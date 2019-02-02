/* Declare dependencies between CPUIDs */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <asm/cpufeature.h>

struct cpuid_dep {
	unsigned int	feature;
	unsigned int	depends;
};

/*
 * Table of CPUID features that depend on others.
 *
 * This only includes dependencies that can be usefully disabled, not
 * features part of the base set (like FPU).
 *
 * Note this all is not __init / __initdata because it can be
 * called from cpu hotplug. It shouldn't do anything in this case,
 * but it's difficult to tell that to the init reference checker.
 */
static const struct cpuid_dep cpuid_deps[] = {
	{ X86_FEATURE_XSAVEOPT,		X86_FEATURE_XSAVE     },
	{ X86_FEATURE_XSAVEC,		X86_FEATURE_XSAVE     },
	{ X86_FEATURE_XSAVES,		X86_FEATURE_XSAVE     },
	{ X86_FEATURE_AVX,		X86_FEATURE_XSAVE     },
	{ X86_FEATURE_PKU,		X86_FEATURE_XSAVE     },
	{ X86_FEATURE_MPX,		X86_FEATURE_XSAVE     },
	{ X86_FEATURE_XGETBV1,		X86_FEATURE_XSAVE     },
	{ X86_FEATURE_FXSR_OPT,		X86_FEATURE_FXSR      },
	{ X86_FEATURE_XMM,		X86_FEATURE_FXSR      },
	{ X86_FEATURE_XMM2,		X86_FEATURE_XMM       },
	{ X86_FEATURE_XMM3,		X86_FEATURE_XMM2      },
	{ X86_FEATURE_XMM4_1,		X86_FEATURE_XMM2      },
	{ X86_FEATURE_XMM4_2,		X86_FEATURE_XMM2      },
	{ X86_FEATURE_XMM3,		X86_FEATURE_XMM2      },
	{ X86_FEATURE_PCLMULQDQ,	X86_FEATURE_XMM2      },
	{ X86_FEATURE_SSSE3,		X86_FEATURE_XMM2,     },
	{ X86_FEATURE_F16C,		X86_FEATURE_XMM2,     },
	{ X86_FEATURE_AES,		X86_FEATURE_XMM2      },
	{ X86_FEATURE_SHA_NI,		X86_FEATURE_XMM2      },
	{ X86_FEATURE_FMA,		X86_FEATURE_AVX       },
	{ X86_FEATURE_AVX2,		X86_FEATURE_AVX,      },
	{ X86_FEATURE_AVX512F,		X86_FEATURE_AVX,      },
	{ X86_FEATURE_AVX512IFMA,	X86_FEATURE_AVX512F   },
	{ X86_FEATURE_AVX512PF,		X86_FEATURE_AVX512F   },
	{ X86_FEATURE_AVX512ER,		X86_FEATURE_AVX512F   },
	{ X86_FEATURE_AVX512CD,		X86_FEATURE_AVX512F   },
	{ X86_FEATURE_AVX512DQ,		X86_FEATURE_AVX512F   },
	{ X86_FEATURE_AVX512BW,		X86_FEATURE_AVX512F   },
	{ X86_FEATURE_AVX512VL,		X86_FEATURE_AVX512F   },
	{ X86_FEATURE_AVX512VBMI,	X86_FEATURE_AVX512F   },
	{ X86_FEATURE_AVX512_VBMI2,	X86_FEATURE_AVX512VL  },
	{ X86_FEATURE_GFNI,		X86_FEATURE_AVX512VL  },
	{ X86_FEATURE_VAES,		X86_FEATURE_AVX512VL  },
	{ X86_FEATURE_VPCLMULQDQ,	X86_FEATURE_AVX512VL  },
	{ X86_FEATURE_AVX512_VNNI,	X86_FEATURE_AVX512VL  },
	{ X86_FEATURE_AVX512_BITALG,	X86_FEATURE_AVX512VL  },
	{ X86_FEATURE_AVX512_4VNNIW,	X86_FEATURE_AVX512F   },
	{ X86_FEATURE_AVX512_4FMAPS,	X86_FEATURE_AVX512F   },
	{ X86_FEATURE_AVX512_VPOPCNTDQ, X86_FEATURE_AVX512F   },
	{}
};

static inline void
setup_feature(struct cpuinfo_x86 *c, unsigned int feature, bool enable)
{
	/*
	 * Note: This could use the non atomic __*_bit() variants, but the
	 * rest of the cpufeature code uses atomics as well, so keep it for
	 * consistency. Cleanup all of it separately.
	 */
	if (enable) {
		/* Set the feature */
		if (!c) {
			set_cpu_cap(&boot_cpu_data, feature);
			clear_bit(feature, (unsigned long *)cpu_caps_cleared);
			setup_force_cpu_cap(feature);
		} else {
			set_bit(feature, (unsigned long *)c->x86_capability);
		}
	} else {
		/* Clear the feature */
		if (!c) {
			clear_cpu_cap(&boot_cpu_data, feature);
			set_bit(feature, (unsigned long *)cpu_caps_cleared);
		} else {
			clear_bit(feature, (unsigned long *)c->x86_capability);
		}
	}
}

/* Take the capabilities and the BUG bits into account */
#define MAX_FEATURE_BITS ((NCAPINTS + NBUGINTS) * sizeof(u32) * 8)

static void
do_setup_cpu_cap(struct cpuinfo_x86 *c, unsigned int feature, bool enable)
{
	DECLARE_BITMAP(disable, MAX_FEATURE_BITS);
	const struct cpuid_dep *d;
	bool changed;

	if (WARN_ON(feature >= MAX_FEATURE_BITS))
		return;

	setup_feature(c, feature, enable);

	/* Collect all features to disable, handling dependencies */
	memset(disable, 0, sizeof(disable));
	__set_bit(feature, disable);

	/* Loop until we get a stable state. */
	do {
		changed = false;
		for (d = cpuid_deps; d->feature; d++) {
			if (!test_bit(d->depends, disable))
				continue;
			if (__test_and_set_bit(d->feature, disable))
				continue;

			changed = true;
			setup_feature(c, d->feature, enable);
		}
	} while (changed);
}

void clear_cpu_cap(struct cpuinfo_x86 *c, unsigned int feature)
{
	/* Disable the feature. */
	do_setup_cpu_cap(c, feature, false);
}

void setup_clear_cpu_cap(unsigned int feature)
{
	/* Disable the feature. */
	do_setup_cpu_cap(NULL, feature, false);
}

void setup_set_cpu_cap(unsigned int feature)
{
	/* Enable the feature. */
	do_setup_cpu_cap(NULL, feature, true);
}

/**
 * find_cpu_cap - Given a cap flag string, find its corresponding feature bit.
 * @cap_flag:	cap flag string as defined in x86_cap_flags[]
 * @pfeature:	feature bit
 *
 * Return: true if the feature is found. false if not found
 */
bool find_cpu_cap(char *cap_flag, unsigned int *pfeature)
{
#ifdef CONFIG_X86_FEATURE_NAMES
	unsigned int feature;

	for (feature = 0; feature < NCAPINTS * 32; feature++) {
		if (!x86_cap_flags[feature])
			continue;

		if (strcmp(cap_flag, x86_cap_flags[feature]) == 0) {
			*pfeature = feature;

			return true;
		}
	}
#endif
	return false;
}
