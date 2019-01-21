/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM dma_fence

#if !defined(_TRACE_FENCE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_DMA_FENCE_H

#include <linux/tracepoint.h>

struct dma_fence;

/*
 * dma-fence tracing *or* How to debug fences
 *
 * The dma-fence tracing provides insight into the user <-> HW execution
 * flow, although dma-fence is not tied to HW and may be used to coordinate
 * any execution flow. A dma-fence represents a job along a timeline (the fence
 * context), and when the job is complete it is signaled. However, for the
 * purposes of visualisation of HW execution, we need a little more information
 * than that as we need to not only know when the job became ready for execution
 * (was passed into the HW queue) but ideally when and on which HW engine that
 * job was eventually scheduled. For CPU bound execution flows, similarly
 * knowing on which CPU the job was scheduled can be vital information for
 * debugging.
 *
 * The typical flow of events from user to HW for a dma-fence would be:
 *
 *   1. dma_fence_init
 *   2. dma_fence_await (optional)
 *      - records the dependencies between fences that must be signaled
 *        before this fence is ready for execution; an asynchronous wait
 *   3. dma_fence_emit
 *      - the fence is ready for execution and passed to the execution queue
 *        (the user to HW/FW/backend transition)
 *   4. dma_fence_execute_start (optional)
 *      - records the start of execution on the backend (HW) and includes
 *        a tag to uniquely identify the backend engine so that concurrent
 *        execution can be traced
 *      - may only be emitted for the first fence in a context to begin
 *        execution
 *   5. dma_fence_execute_end (optional)
 *     - records the corresponding completion point of backend execution
 *     - may only be emitted for the last fence in a context to finish
 *       execution
 *   6. dma_fence_signaled
 *     - records when the fence was marked as completed and the result
 *       propagated to the various waiters
 *   7. dma_fence_destroy
 *
 * Note that not all fences are used in conjunction with HW engines, and
 * so may exclude the execution tracing. Nor do they all correspond with
 * client API, although many may be used as background tasks required
 * before HW execution.
 *
 * The flow of events from HW to user would be:
 *
 *   1. dma_fence_wait_begin
 *   2. dma_fence_enable_signaling (optional)
 *   3. dma_fence_signaled
 *   4. dma_fence_wait_end
 *
 * For improved visualisation, dma_fence_context_create and
 * dma_fence_context_destroy are used to couple the context id to a string.
 */

TRACE_EVENT(dma_fence_context_create,
	    TP_PROTO(u64 context, const char *driver, const char *timeline),
	    TP_ARGS(context, driver, timeline),

	    TP_STRUCT__entry(
			     __field(u64, context)
			     __string(driver, driver)
			     __string(timeline, timeline)
			     ),

	    TP_fast_assign(
			   __entry->context = context;
			   __assign_str(driver, driver)
			   __assign_str(timeline, timeline)
			   ),

	    TP_printk("context=%llu, driver=%s, timeline=%s",
		      __entry->context, __get_str(driver), __get_str(timeline))
);

TRACE_EVENT(dma_fence_context_destroy,
	    TP_PROTO(u64 context),
	    TP_ARGS(context),

	    TP_STRUCT__entry(
			     __field(u64, context)
			     ),

	    TP_fast_assign(
			   __entry->context = context;
			   ),

	    TP_printk("context=%llu", __entry->context)
);

DECLARE_EVENT_CLASS(dma_fence,

	TP_PROTO(struct dma_fence *fence),

	TP_ARGS(fence),

	TP_STRUCT__entry(
		__field(u64, context)
		__field(u64, seqno)
	),

	TP_fast_assign(
		__entry->context = fence->context;
		__entry->seqno = fence->seqno;
	),

	TP_printk("context=%llu, seqno=%llu", __entry->context, __entry->seqno)
);

TRACE_EVENT(dma_fence_init,
	TP_PROTO(struct dma_fence *fence),

	TP_ARGS(fence),

	TP_STRUCT__entry(
		__string(driver, fence->ops->get_driver_name(fence))
		__string(timeline, fence->ops->get_timeline_name(fence))
		__field(unsigned int, context)
		__field(unsigned int, seqno)
	),

	TP_fast_assign(
		__assign_str(driver, fence->ops->get_driver_name(fence))
		__assign_str(timeline, fence->ops->get_timeline_name(fence))
		__entry->context = fence->context;
		__entry->seqno = fence->seqno;
	),

	TP_printk("driver=%s timeline=%s context=%u seqno=%u",
		  __get_str(driver), __get_str(timeline), __entry->context,
		  __entry->seqno)
);

DEFINE_EVENT(dma_fence, dma_fence_emit,

	TP_PROTO(struct dma_fence *fence),

	TP_ARGS(fence)
);

DEFINE_EVENT(dma_fence, dma_fence_destroy,

	TP_PROTO(struct dma_fence *fence),

	TP_ARGS(fence)
);

DEFINE_EVENT(dma_fence, dma_fence_enable_signal,

	TP_PROTO(struct dma_fence *fence),

	TP_ARGS(fence)
);

DEFINE_EVENT(dma_fence, dma_fence_signaled,

	TP_PROTO(struct dma_fence *fence),

	TP_ARGS(fence)
);

DEFINE_EVENT(dma_fence, dma_fence_wait_start,

	TP_PROTO(struct dma_fence *fence),

	TP_ARGS(fence)
);

DEFINE_EVENT(dma_fence, dma_fence_wait_end,

	TP_PROTO(struct dma_fence *fence),

	TP_ARGS(fence)
);

TRACE_EVENT(dma_fence_await,
	    TP_PROTO(struct dma_fence *wait, struct dma_fence *signal),
	    TP_ARGS(wait, signal),

	    TP_STRUCT__entry(
			     __field(u64, wait_context)
			     __field(u64, wait_seqno)
			     __field(u64, signal_context)
			     __field(u64, signal_seqno)
			     ),

	    TP_fast_assign(
			   __entry->wait_context = wait->context;
			   __entry->wait_seqno = wait->seqno;
			   __entry->signal_context = signal->context;
			   __entry->signal_seqno = signal->seqno;
			   ),

	    TP_printk("wait_context=%llu, wait_seqno=%llu, signal_context=%llu, signal_seqno=%llu",
		      __entry->wait_context, __entry->wait_seqno,
		      __entry->signal_context, __entry->signal_seqno)
);

TRACE_EVENT(dma_fence_execute_start,
	    TP_PROTO(struct dma_fence *fence, u64 hwid),
	    TP_ARGS(fence, hwid),

	    TP_STRUCT__entry(
			     __field(u64, context)
			     __field(u64, seqno)
			     __field(u64, hwid)
			     ),

	    TP_fast_assign(
			   __entry->context = fence->context;
			   __entry->seqno = fence->seqno;
			   __entry->hwid = hwid;
			   ),

	    TP_printk("context=%llu, seqno=%llu, hwid=%llu",
		      __entry->context, __entry->seqno, __entry->hwid)
);

TRACE_EVENT(dma_fence_execute_end,
	    TP_PROTO(struct dma_fence *fence, u64 hwid),
	    TP_ARGS(fence, hwid),

	    TP_STRUCT__entry(
			     __field(u64, context)
			     __field(u64, seqno)
			     __field(u64, hwid)
			     ),

	    TP_fast_assign(
			   __entry->context = fence->context;
			   __entry->seqno = fence->seqno;
			   __entry->hwid = hwid;
			   ),

	    TP_printk("context=%llu, seqno=%llu, hwid=%llu",
		      __entry->context, __entry->seqno, __entry->hwid)
);

#endif /*  _TRACE_DMA_FENCE_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
