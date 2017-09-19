/* Declare dependencies between CPUIDs */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <asm/cpufeature.h>

struct cpuid_dep {
	int feature;
	int disable;
};

/*
 * Table of CPUID features that depend on others.
 *
 * This only includes dependencies that can be usefully disabled, not
 * features part of the base set (like FPU).
 */
const static struct cpuid_dep cpuid_deps[] = {
	{ X86_FEATURE_XSAVE,   X86_FEATURE_XSAVEOPT },
	{ X86_FEATURE_XSAVE,   X86_FEATURE_XSAVEC },
	{ X86_FEATURE_XSAVE,   X86_FEATURE_XSAVES },
	{ X86_FEATURE_XSAVE,   X86_FEATURE_AVX },
	{ X86_FEATURE_XSAVE,   X86_FEATURE_AVX512F },
	{ X86_FEATURE_XSAVE,   X86_FEATURE_PKU },
	{ X86_FEATURE_XSAVE,   X86_FEATURE_MPX },
	{ X86_FEATURE_XSAVE,   X86_FEATURE_XGETBV1 },
	{ X86_FEATURE_XMM,     X86_FEATURE_XMM2 },
	{ X86_FEATURE_XMM2,    X86_FEATURE_XMM3 },
	{ X86_FEATURE_XMM2,    X86_FEATURE_XMM4_1 },
	{ X86_FEATURE_XMM2,    X86_FEATURE_XMM4_2 },
	{ X86_FEATURE_XMM2,    X86_FEATURE_XMM3 },
	{ X86_FEATURE_XMM2,    X86_FEATURE_PCLMULQDQ },
	{ X86_FEATURE_XMM2,    X86_FEATURE_SSSE3 },
	{ X86_FEATURE_XMM2,    X86_FEATURE_F16C },
	{ X86_FEATURE_XMM2,    X86_FEATURE_AES },
	{ X86_FEATURE_FMA,     X86_FEATURE_AVX },
	{ X86_FEATURE_AVX512F, X86_FEATURE_AVX512IFMA },
	{ X86_FEATURE_AVX512F, X86_FEATURE_AVX512PF },
	{ X86_FEATURE_AVX512F, X86_FEATURE_AVX512ER },
	{ X86_FEATURE_AVX512F, X86_FEATURE_AVX512CD },
	{ X86_FEATURE_AVX512F, X86_FEATURE_AVX512DQ },
	{ X86_FEATURE_AVX512F, X86_FEATURE_AVX512BW },
	{ X86_FEATURE_AVX512F, X86_FEATURE_AVX512VL },
	{ X86_FEATURE_AVX512F, X86_FEATURE_AVX512VBMI },
	{ X86_FEATURE_AVX512F, X86_FEATURE_AVX512_4VNNIW },
	{ X86_FEATURE_AVX512F, X86_FEATURE_AVX512_4FMAPS },
	{ X86_FEATURE_AVX512F, X86_FEATURE_AVX512_VPOPCNTDQ },
	{ X86_FEATURE_AVX,     X86_FEATURE_AVX2 },
	{}
};

static inline void clearfeat(struct cpuinfo_x86 *cpu, int feat)
{
	if (!cpu)
		__setup_clear_cpu_cap(feat);
	else
		__clear_cpu_cap(cpu, feat);
}

static void do_clear_cpu_cap(struct cpuinfo_x86 *cpu, int feat)
{
	bool changed;
	__u32 disable[NCAPINTS + NBUGINTS];
	unsigned long *disable_mask = (unsigned long *)disable;
	const struct cpuid_dep *d;

	clearfeat(cpu, feat);

	/* Collect all features to disable, handling dependencies */
	memset(disable, 0, sizeof(disable));
	__set_bit(feat, disable_mask);
	do {
		changed = false;
		for (d = cpuid_deps; d->feature; d++) {
			if (test_bit(d->feature, disable_mask) &&
			    !__test_and_set_bit(d->disable, disable_mask)) {
				changed = true;
				clearfeat(cpu, d->disable);
			}
		}
	} while (changed);
}

void clear_cpu_cap(struct cpuinfo_x86 *cpu, int feat)
{
	do_clear_cpu_cap(cpu, feat);
}

EXPORT_SYMBOL_GPL(clear_cpu_cap);

void setup_clear_cpu_cap(int feat)
{
	do_clear_cpu_cap(NULL, feat);
}
