#include "unwind.h"
#include "thread.h"

int unwind__prepare_access(struct thread *thread)
{
	register_local_unwind_libunwind_ops(thread);

	return thread->unwind_libunwind_ops->prepare_access(thread);
}

void unwind__flush_access(struct thread *thread)
{
	if (thread->unwind_libunwind_ops)
		thread->unwind_libunwind_ops->flush_access(thread);
}

void unwind__finish_access(struct thread *thread)
{
	if (thread->unwind_libunwind_ops)
		thread->unwind_libunwind_ops->finish_access(thread);
}

int unwind__get_entries(unwind_entry_cb_t cb, void *arg,
			 struct thread *thread,
			 struct perf_sample *data, int max_stack)
{
	if (thread->unwind_libunwind_ops)
		return thread->unwind_libunwind_ops->get_entries(cb, arg,
								 thread,
								 data,
								 max_stack);
	else
		return 0;
}
