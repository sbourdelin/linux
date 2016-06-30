#ifndef ARCH_PERF_COMMON_H
#define ARCH_PERF_COMMON_H

#include "../util/env.h"

extern const char *objdump_path;

/* Macro for normalized arch names */
#define NORM_X86	"x86"
#define NORM_SPARC	"sparc"
#define NORM_ARM64	"arm64"
#define NORM_ARM	"arm"
#define NORM_S390	"s390"
#define NORM_PARISC	"parisc"
#define NORM_POWERPC	"powerpc"
#define NORM_MIPS	"mips"
#define NORM_SH		"sh"

int perf_env__lookup_objdump(struct perf_env *env);
const char *normalize_arch(char *arch);

#endif /* ARCH_PERF_COMMON_H */
