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

#include "i915_drv.h"

static void intel_breadcrumbs_fake_irq(unsigned long data)
{
	struct intel_breadcrumbs *b = (struct intel_breadcrumbs *)data;
	struct task_struct *task;

	/*
	 * The timer persists in case we cannot enable interrupts,
	 * or if we have previously seen seqno/interrupt incoherency
	 * ("missed interrupt" syndrome). Here the worker will wake up
	 * every jiffie in order to kick the oldest waiter to do the
	 * coherent seqno check.
	 */

	task = READ_ONCE(b->first_waiter);
	if (task) {
		wake_up_process(task);
		mod_timer(&b->fake_irq, jiffies + 1);
	}
}

static void irq_enable(struct intel_engine_cs *engine)
{
	WARN_ON(!engine->irq_get(engine));
}

static void irq_disable(struct intel_engine_cs *engine)
{
	engine->irq_put(engine);
}

static void __intel_breadcrumbs_enable_irq(struct intel_breadcrumbs *b)
{
	struct intel_engine_cs *engine =
		container_of(b, struct intel_engine_cs, breadcrumbs);
	bool noirq;

	if (b->rpm_wakelock)
		return;

	/* Since we are waiting on a request, the GPU should be busy
	 * and should have its own rpm reference. For completeness,
	 * record an rpm reference for ourselves to cover the
	 * interrupt we unmask.
	 */
	intel_runtime_pm_get_noresume(engine->i915);
	b->rpm_wakelock = true;

	/* No interrupts? Kick the waiter every jiffie! */
	noirq = true;
	if (intel_irqs_enabled(engine->i915)) {
		noirq = test_bit(engine->id,
				 &engine->i915->gpu_error.missed_irq_rings);
		if (!test_bit(engine->id,
			      &engine->i915->gpu_error.test_irq_rings)) {
			irq_enable(engine);
			b->irq_enabled = true;
		}
	}
	if (noirq)
		mod_timer(&b->fake_irq, jiffies + 1);
}

static void __intel_breadcrumbs_disable_irq(struct intel_breadcrumbs *b)
{
	struct intel_engine_cs *engine =
		container_of(b, struct intel_engine_cs, breadcrumbs);

	if (!b->rpm_wakelock)
		return;

	if (b->irq_enabled) {
		irq_disable(engine);
		b->irq_enabled = false;
	}

	intel_runtime_pm_put(engine->i915);
	b->rpm_wakelock = false;
}

inline struct intel_breadcrumb *to_crumb(struct rb_node *node)
{
	return container_of(node, struct intel_breadcrumb, node);
}

bool intel_engine_add_breadcrumb(struct intel_engine_cs *engine,
				 struct intel_breadcrumb *wait)
{
	struct intel_breadcrumbs *b = &engine->breadcrumbs;
	u32 seqno = intel_ring_get_seqno(engine);
	struct rb_node **p, *parent, *completed;
	bool first;

	spin_lock(&b->lock);

	/* Insert the request into the retirment ordered list
	 * of waiters by walking the rbtree. If we are the oldest
	 * seqno in the tree (the first to be retired), then
	 * set ourselves as the bottom-half.
	 *
	 * As we descend the tree, prune completed branches since we hold the
	 * spinlock we know that the first_waiter must be delayed and can
	 * reduce some of the sequential wake up latency if we take action
	 * ourselves and wake up the copmleted tasks in parallel.
	 */
	first = true;
	parent = NULL;
	completed = NULL;
	p = &b->requests.rb_node;
	while (*p) {
		parent = *p;
		if (i915_seqno_passed(wait->seqno, to_crumb(parent)->seqno)) {
			p = &parent->rb_right;
			if (i915_seqno_passed(seqno, to_crumb(parent)->seqno))
				completed = parent;
			else
				first = false;
		} else
			p = &parent->rb_left;
	}
	rb_link_node(&wait->node, parent, p);
	rb_insert_color(&wait->node, &b->requests);

	if (completed != NULL) {
		struct rb_node *next = rb_next(completed);

		if (next && next != &wait->node) {
			smp_store_mb(b->first_waiter, to_crumb(next)->task);
			__intel_breadcrumbs_enable_irq(b);
			wake_up_process(to_crumb(next)->task);
		}

		do {
			struct intel_breadcrumb *crumb = to_crumb(completed);
			completed = rb_prev(completed);

			rb_erase(&crumb->node, &b->requests);
			RB_CLEAR_NODE(&crumb->node);
			wake_up_process(crumb->task);
		} while (completed != NULL);
	}

	if (first)
		smp_store_mb(b->first_waiter, wait->task);
	BUG_ON(b->first_waiter == NULL);

	spin_unlock(&b->lock);

	return first;
}

void intel_engine_enable_breadcrumb_irq(struct intel_engine_cs *engine)
{
	struct intel_breadcrumbs *b = &engine->breadcrumbs;

	spin_lock(&b->lock);
	__intel_breadcrumbs_enable_irq(b);
	spin_unlock(&b->lock);
}

void intel_engine_enable_fake_irq(struct intel_engine_cs *engine)
{
	mod_timer(&engine->breadcrumbs.fake_irq, jiffies + 1);
}

void intel_engine_remove_breadcrumb(struct intel_engine_cs *engine,
				    struct intel_breadcrumb *wait)
{
	struct intel_breadcrumbs *b = &engine->breadcrumbs;

	/* Quick check to see if this waiter was already decoupled from
	 * the tree by the bottom-half to avoid contention on the spinlock
	 * by the herd.
	 */
	if (RB_EMPTY_NODE(&wait->node))
		return;

	spin_lock(&b->lock);

	if (b->first_waiter == wait->task) {
		struct rb_node *next;
		struct task_struct *task;

		/* We are the current bottom-half. Find the next candidate,
		 * the first waiter in the queue on the remaining oldest
		 * request. As multiple seqnos may complete in the time it
		 * takes us to wake up and find the next waiter, we have to
		 * wake up that waiter for it to perform its own coherent
		 * completion check.
		 */
		next = rb_next(&wait->node);
		if (next) {
			/* If the next waiter is already complete,
			 * wake it up and continue onto the next waiter. So
			 * if have a small herd, they will wake up in parallel
			 * rather than sequentially, which should reduce
			 * the overall latency in waking all the completed
			 * clients.
			 */
			u32 seqno = intel_ring_get_seqno(engine);
			while (i915_seqno_passed(seqno,
						 to_crumb(next)->seqno)) {
				struct rb_node *n = rb_next(next);

				rb_erase(next, &b->requests);
				RB_CLEAR_NODE(next);
				wake_up_process(to_crumb(next)->task);

				next = n;
				if (next == NULL)
					break;
			}
		}
		task = next ? to_crumb(next)->task : NULL;

		smp_store_mb(b->first_waiter, task);
		if (task) {
			/* In our haste, we may have completed the first waiter
			 * before we enabled the interrupt. Do so now as we
			 * have a second waiter for a future seqno. Afterwards,
			 * we have to wake up that waiter in case we missed
			 * the interrupt, or if we have to handle an
			 * exception rather than a seqno completion.
			 */
			__intel_breadcrumbs_enable_irq(b);
			wake_up_process(task);
		} else
			__intel_breadcrumbs_disable_irq(b);
	}

	if (!RB_EMPTY_NODE(&wait->node))
		rb_erase(&wait->node, &b->requests);
	spin_unlock(&b->lock);
}

void intel_engine_init_breadcrumbs(struct intel_engine_cs *engine)
{
	struct intel_breadcrumbs *b = &engine->breadcrumbs;

	spin_lock_init(&b->lock);
	setup_timer(&b->fake_irq, intel_breadcrumbs_fake_irq, (unsigned long)b);
}

void intel_engine_fini_breadcrumbs(struct intel_engine_cs *engine)
{
	struct intel_breadcrumbs *b = &engine->breadcrumbs;

	del_timer_sync(&b->fake_irq);
}
