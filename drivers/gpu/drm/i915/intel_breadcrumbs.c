/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <linux/kthread.h>

#include "i915_drv.h"

static bool __irq_enable(struct intel_engine_cs *engine)
{
	if (test_bit(engine->id, &engine->i915->gpu_error.test_irq_rings))
		return false;

	if (!intel_irqs_enabled(engine->i915))
		return false;

	return engine->irq_get(engine);
}

static struct drm_i915_gem_request *to_request(struct rb_node *rb)
{
	if (rb == NULL)
		return NULL;

	return rb_entry(rb, struct drm_i915_gem_request, irq_node);
}

/*
 * intel_breadcrumbs_irq() acts as the bottom-half for the user interrupt,
 * which we use as the breadcrumb after every request. In order to scale
 * to many concurrent waiters, we delegate the task of reading the coherent
 * seqno to this bottom-half. (Otherwise every waiter is woken after each
 * interrupt and they all attempt to perform the heavyweight coherent
 * seqno check.) Individual clients register with the bottom-half when
 * waiting on a request, and are then woken when the bottom-half notices
 * their seqno is complete. This incurs an extra context switch from the
 * interrupt to the client in the uncontended case.
 */
static int intel_breadcrumbs_irq(void *data)
{
	struct drm_i915_private *i915 = data;
	struct intel_breadcrumbs *b = &i915->breadcrumbs;
	unsigned irq_get = 0;
	unsigned irq_enabled = 0;
	int i;

	while (!kthread_should_stop()) {
		unsigned reset_counter = i915_reset_counter(&i915->gpu_error);
		unsigned long timeout_jiffies;
		bool idling = false;

		/* On every tick, walk the seqno-ordered list of requests
		 * and for each retired request wakeup its clients. If we
		 * find an unfinished request, go back to sleep.
		 */
		set_current_state(TASK_INTERRUPTIBLE);

		/* Note carefully that we do not hold a reference for the
		 * requests on this list. Therefore we only inspect them
		 * whilst holding the spinlock to ensure that they are not
		 * freed in the meantime, and the client must remove the
		 * request from the list if it is interrupted (before it
		 * itself releases its reference).
		 */
		spin_lock(&b->lock);
		for (i = 0; i < I915_NUM_RINGS; i++) {
			struct intel_engine_cs *engine = &i915->ring[i];
			struct intel_breadcrumbs_engine *be = &b->engine[i];
			struct drm_i915_gem_request *request = be->first;

			if (request == NULL) {
				if ((irq_get & (1 << i))) {
					if (irq_enabled & (1 << i)) {
						engine->irq_put(engine);
						irq_enabled &= ~ (1 << i);
					}
					intel_runtime_pm_put(i915);
					irq_get &= ~(1 << i);
				}
				continue;
			}

			if ((irq_get & (1 << i)) == 0) {
				intel_runtime_pm_get(i915);
				irq_enabled |= __irq_enable(engine) << i;
				irq_get |= 1 << i;
			}

			do {
				struct rb_node *next;

				if (request->reset_counter == reset_counter &&
				    !i915_gem_request_completed(request, false))
					break;

				next = rb_next(&request->irq_node);
				rb_erase(&request->irq_node, &be->requests);
				RB_CLEAR_NODE(&request->irq_node);

				wake_up_all(&request->wait);

				request = to_request(next);
			} while (request);
			be->first = request;
			idling |= request == NULL;

			/* Make sure the hangcheck timer is armed in case
			 * the GPU hangs and we are never woken up.
			 */
			if (request)
				i915_queue_hangcheck(i915);
		}
		spin_unlock(&b->lock);

		/* If we don't have interrupts available (either we have
		 * detected the hardware is missing interrupts or the
		 * interrupt delivery was disabled by the user), wake up
		 * every jiffie to check for request completion.
		 *
		 * If we have processed all requests for one engine, we
		 * also wish to wake up in a jiffie to turn off the
		 * breadcrumb interrupt for that engine. We delay
		 * switching off the interrupt in order to allow another
		 * waiter to start without incurring additional latency
		 * enabling the interrupt.
		 */
		timeout_jiffies = MAX_SCHEDULE_TIMEOUT;
		if (idling || i915->gpu_error.missed_irq_rings & irq_enabled)
			timeout_jiffies = 1;

		/* Unlike the individual clients, we do not want this
		 * background thread to contribute to the system load,
		 * i.e. we do not want to use io_schedule() here.
		 */
		schedule_timeout(timeout_jiffies);
	}

	for (i = 0; i < I915_NUM_RINGS; i++) {
		if ((irq_get & (1 << i))) {
			if (irq_enabled & (1 << i)) {
				struct intel_engine_cs *engine = &i915->ring[i];
				engine->irq_put(engine);
			}
			intel_runtime_pm_put(i915);
		}
	}

	return 0;
}

bool intel_breadcrumbs_add_waiter(struct drm_i915_gem_request *request)
{
	struct intel_breadcrumbs *b = &request->i915->breadcrumbs;
	bool first = false;

	spin_lock(&b->lock);
	if (request->irq_count++ == 0) {
		struct intel_breadcrumbs_engine *be =
			&b->engine[request->ring->id];
		struct rb_node **p, *parent;

		if (be->first == NULL)
			wake_up_process(b->task);

		init_waitqueue_head(&request->wait);

		first = true;
		parent = NULL;
		p = &be->requests.rb_node;
		while (*p) {
			struct drm_i915_gem_request *__req;

			parent = *p;
			__req = rb_entry(parent, typeof(*__req), irq_node);

			if (i915_seqno_passed(request->seqno, __req->seqno)) {
				p = &parent->rb_right;
				first = false;
			} else
				p = &parent->rb_left;
		}
		if (first)
			be->first = request;

		rb_link_node(&request->irq_node, parent, p);
		rb_insert_color(&request->irq_node, &be->requests);
	}
	spin_unlock(&b->lock);

	return first;
}

void intel_breadcrumbs_remove_waiter(struct drm_i915_gem_request *request)
{
	struct intel_breadcrumbs *b = &request->i915->breadcrumbs;

	spin_lock(&b->lock);
	if (--request->irq_count == 0 && !RB_EMPTY_NODE(&request->irq_node)) {
		struct intel_breadcrumbs_engine *be =
			&b->engine[request->ring->id];
		if (be->first == request)
			be->first = to_request(rb_next(&request->irq_node));
		rb_erase(&request->irq_node, &be->requests);
	}
	spin_unlock(&b->lock);
}

int intel_breadcrumbs_init(struct drm_i915_private *i915)
{
	struct intel_breadcrumbs *b = &i915->breadcrumbs;
	struct task_struct *task;

	spin_lock_init(&b->lock);

	task = kthread_run(intel_breadcrumbs_irq, i915, "irq/i915");
	if (IS_ERR(task))
		return PTR_ERR(task);

	b->task = task;

	return 0;
}

void intel_breadcrumbs_fini(struct drm_i915_private *i915)
{
	struct intel_breadcrumbs *b = &i915->breadcrumbs;

	if (b->task == NULL)
		return;

	kthread_stop(b->task);
	b->task = NULL;
}
