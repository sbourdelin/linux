#include "thread.h"
#include "session.h"
#include "unwind.h"
#include "symbol.h"
#include "debug.h"
#include "arch/common.h"

void unwind__get_arch(struct thread *thread, struct map *map)
{
	const char *arch;
	enum dso_type dso_type;

	if (!thread->mg->machine->env)
		return;

	dso_type = dso__type(map->dso, thread->mg->machine);
	if (dso_type == DSO__TYPE_UNKNOWN)
		return;

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
}
