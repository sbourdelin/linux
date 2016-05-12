#include "thread.h"
#include "session.h"
#include "unwind.h"
#include "symbol.h"
#include "debug.h"

static int __null__prepare_access(struct thread *thread __maybe_unused)
{
	return 0;
}

static void __null__flush_access(struct thread *thread __maybe_unused)
{
}

static void __null__finish_access(struct thread *thread __maybe_unused)
{
}

static int __null__get_entries(unwind_entry_cb_t cb __maybe_unused,
			       void *arg __maybe_unused,
			       struct thread *thread __maybe_unused,
			       struct perf_sample *data __maybe_unused,
			       int max_stack __maybe_unused)
{
	return 0;
}

static struct unwind_libunwind_ops null_unwind_libunwind_ops = {
	.prepare_access = __null__prepare_access,
	.flush_access   = __null__flush_access,
	.finish_access  = __null__finish_access,
	.get_entries    = __null__get_entries,
};

void register_null_unwind_libunwind_ops(struct thread *thread)
{
	thread->unwind_libunwind_ops = &null_unwind_libunwind_ops;
	if (thread->mg)
		pr_err("unwind: target platform=%s unwind unsupported\n",
		       thread->mg->machine->env->arch);
}

void register_unwind_libunwind_ops(struct unwind_libunwind_ops *ops,
				   struct thread *thread)
{
	thread->unwind_libunwind_ops = ops;
}

void unwind__flush_access(struct thread *thread)
{
	thread->unwind_libunwind_ops->flush_access(thread);
}

void unwind__finish_access(struct thread *thread)
{
	thread->unwind_libunwind_ops->finish_access(thread);
}

void unwind__get_arch(struct thread *thread, struct map *map)
{
	char *arch;
	enum dso_type dso_type;
	int use_local_unwind = 0;

	if (!thread->mg->machine->env)
		return;

	dso_type = dso__type(map->dso, thread->mg->machine);
	if (dso_type == DSO__TYPE_UNKNOWN)
		return;

	if (thread->addr_space)
		pr_debug("unwind: thread map already set, 64bit is %d, dso=%s\n",
			 dso_type == DSO__TYPE_64BIT, map->dso->name);

	arch = thread->mg->machine->env->arch;

	if (!strcmp(arch, "x86_64")
		   || !strcmp(arch, "x86")
		   || !strcmp(arch, "i686")) {
		pr_debug("unwind: thread map is X86, 64bit is %d\n",
			 dso_type == DSO__TYPE_64BIT);
		if (dso_type != DSO__TYPE_64BIT) {
#ifdef HAVE_LIBUNWIND_X86_SUPPORT
			register_unwind_libunwind_ops(
				&_Ux86_unwind_libunwind_ops, thread);
#else
			register_null_unwind_libunwind_ops(thread);
#endif
		} else
			use_local_unwind = 1;
	} else if (!strcmp(arch, "aarch64") || !strncmp(arch, "arm", 3)) {
		pr_debug("Thread map is ARM, 64bit is %d, dso=%s\n",
			 dso_type == DSO__TYPE_64BIT, map->dso->name);
		if (dso_type == DSO__TYPE_64BIT) {
#ifdef HAVE_LIBUNWIND_AARCH64_SUPPORT
			register_unwind_libunwind_ops(
				&_Uaarch64_unwind_libunwind_ops, thread);
#else
			register_null_unwind_libunwind_ops(thread);
#endif
		} else
			use_local_unwind = 1;
	} else {
		use_local_unwind = 1;
	}

	if (use_local_unwind)
		register_local_unwind_libunwind_ops(thread);

	if (thread->unwind_libunwind_ops->prepare_access(thread) < 0)
		return;
}
