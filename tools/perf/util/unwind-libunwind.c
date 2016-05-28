#include "unwind.h"
#include "thread.h"
#include "session.h"
#include "debug.h"
#include "arch/common.h"

int unwind__prepare_access(struct thread *thread, struct map *map)
{
	const char *arch;
	enum dso_type dso_type;

	if (!thread->mg->machine->env)
		return -1;

	dso_type = dso__type(map->dso, thread->mg->machine);
	if (dso_type == DSO__TYPE_UNKNOWN)
		return -1;

	if (thread->addr_space)
		pr_debug("unwind: thread map already set, 64bit is %d, dso=%s\n",
			 dso_type == DSO__TYPE_64BIT, map->dso->name);

	arch = normalize_arch(thread->mg->machine->env->arch);

	if (!strcmp(arch, "x86")) {
		if (dso_type != DSO__TYPE_64BIT)
#ifdef HAVE_LIBUNWIND_X86_SUPPORT
			pr_err("unwind: target platform=%s is not implemented\n", arch);
#else
			pr_err("unwind: target platform=%s is not supported\n", arch);
#endif
	}

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
