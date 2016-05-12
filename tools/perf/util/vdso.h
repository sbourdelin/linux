#ifndef __PERF_VDSO__
#define __PERF_VDSO__

#include <linux/types.h>
#include <string.h>
#include <stdbool.h>
#include "util.h"

#define VDSO__MAP_NAME "[vdso]"

#if BITS_PER_LONG == 64
#define DSO__NAME_VDSO    "[vdso64]"
#else
#define DSO__NAME_VDSO    "[vdso32]"
#endif
#define DSO__NAME_VDSO32  "[vdso32]"
#define DSO__NAME_VDSOX32 "[vdsox32]"

static inline bool is_vdso_map(const char *filename)
{
	return !strcmp(filename, VDSO__MAP_NAME);
}

struct dso;

bool dso__is_vdso(struct dso *dso);

struct machine;
struct thread;

struct dso *machine__findnew_vdso(struct machine *machine, struct thread *thread);
void machine__exit_vdso(struct machine *machine);

#endif /* __PERF_VDSO__ */
