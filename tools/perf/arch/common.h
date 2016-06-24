#ifndef ARCH_PERF_COMMON_H
#define ARCH_PERF_COMMON_H

#include "../util/env.h"

extern const char *objdump_path;

#define PERF_ARCH_X86		"x86"
#define PERF_ARCH_SPARC		"sparc"
#define PERF_ARCH_ARM64		"arm64"
#define PERF_ARCH_ARM		"arm"
#define PERF_ARCH_S390		"s390"
#define PERF_ARCH_PARISC	"parisc"
#define PERF_ARCH_POWERPC	"powerpc"
#define PERF_ARCH_MIPS		"mips"
#define PERF_ARCH_SH		"sh"

int perf_env__lookup_objdump(struct perf_env *env);
const char *normalize_arch(char *arch);

#endif /* ARCH_PERF_COMMON_H */
