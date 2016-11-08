/*
 *	Routines to identify additional cpu features that are scattered in
 *	cpuid space.
 */
#include <linux/cpu.h>

#include <asm/pat.h>
#include <asm/processor.h>

#include <asm/apic.h>

struct cpuid_bit {
	u16 feature;
	u8 reg;
	u8 bit;
	u32 level;
	u32 sub_leaf;
};

enum cpuid_regs {
	CR_EAX = 0,
	CR_ECX,
	CR_EDX,
	CR_EBX
};

struct msr_bit {
	u16 feature;
	u16 msr;
	u8 bit;
};

void init_scattered_cpuid_features(struct cpuinfo_x86 *c)
{
	const struct cpuid_bit *cb;
	const struct msr_bit *mb;
	u32 max_level;
	u32 regs[4];
	u64 msrval;

	static const struct cpuid_bit cpuid_bits[] = {
		{ X86_FEATURE_INTEL_PT,		CR_EBX,25, 0x00000007, 0 },
		{ X86_FEATURE_AVX512_4VNNIW,	CR_EDX, 2, 0x00000007, 0 },
		{ X86_FEATURE_AVX512_4FMAPS,	CR_EDX, 3, 0x00000007, 0 },
		{ X86_FEATURE_APERFMPERF,	CR_ECX, 0, 0x00000006, 0 },
		{ X86_FEATURE_EPB,		CR_ECX, 3, 0x00000006, 0 },
		{ X86_FEATURE_HW_PSTATE,	CR_EDX, 7, 0x80000007, 0 },
		{ X86_FEATURE_CPB,		CR_EDX, 9, 0x80000007, 0 },
		{ X86_FEATURE_PROC_FEEDBACK,	CR_EDX,11, 0x80000007, 0 },
		{ 0, 0, 0, 0, 0 }
	};

	static const struct msr_bit msr_bits[] = {
		{ X86_FEATURE_CPUID_FAULT,	MSR_PLATFORM_INFO, 31 },
		{ 0, 0, 0 }
	};

	for (cb = cpuid_bits; cb->feature; cb++) {

		/* Verify that the level is valid */
		max_level = cpuid_eax(cb->level & 0xffff0000);
		if (max_level < cb->level ||
		    max_level > (cb->level | 0xffff))
			continue;

		cpuid_count(cb->level, cb->sub_leaf, &regs[CR_EAX],
			    &regs[CR_EBX], &regs[CR_ECX], &regs[CR_EDX]);

		if (regs[cb->reg] & (1 << cb->bit))
			set_cpu_cap(c, cb->feature);
	}

	for (mb = msr_bits; mb->feature; mb++) {
		if (rdmsrl_safe(mb->msr, &msrval))
			continue;
		if (msrval & (1ULL << mb->bit))
			set_cpu_cap(c, mb->feature);
	}

	if (cpu_has(c, X86_FEATURE_CPUID_FAULT)) {
		rdmsrl(MSR_MISC_FEATURES_ENABLES, msrval);
		this_cpu_write(msr_misc_features_enables_shadow, msrval);
	}
}
