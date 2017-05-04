/*
 *  Copyright 2017, IBM Corporation
 */

#include <linux/string.h>
#include <linux/sched.h>
#include <linux/threads.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/jump_label.h>

#include <asm/cpufeatures.h>
#include <asm/cputable.h>
#include <asm/prom.h>		/* for PTRRELOC on ARCH=ppc */
#include <asm/oprofile_impl.h>
#include <asm/mmu.h>
#include <asm/setup.h>

#ifdef DEBUG
#define DBG(fmt...) pr_err(fmt)
#else
#define DBG(fmt...)
#endif

#define CPU_FTRS_BASE \
	   (CPU_FTR_USE_TB | \
	    CPU_FTR_LWSYNC | \
	    CPU_FTR_FPU_UNAVAILABLE |\
	    CPU_FTR_NODSISRALIGN |\
	    CPU_FTR_NOEXECUTE |\
	    CPU_FTR_COHERENT_ICACHE | \
	    CPU_FTR_STCX_CHECKS_ADDRESS |\
	    CPU_FTR_POPCNTB | CPU_FTR_POPCNTD | \
	    CPU_FTR_DAWR | \
	    CPU_FTR_ARCH_206 |\
	    CPU_FTR_ARCH_207S)

#define MMU_FTRS_HASH_BASE (MMU_FTRS_POWER8)

#define COMMON_USER_BASE	(PPC_FEATURE_32 | PPC_FEATURE_64 | \
				 PPC_FEATURE_ARCH_2_06 |\
				 PPC_FEATURE_ICACHE_SNOOP)
#define COMMON_USER2_BASE	(PPC_FEATURE2_ARCH_2_07 | \
				 PPC_FEATURE2_ISEL)
/*
 * Set up the base CPU
 */

extern void __flush_tlb_power8(unsigned int action);
extern void __flush_tlb_power9(unsigned int action);
extern long __machine_check_early_realmode_p8(struct pt_regs *regs);
extern long __machine_check_early_realmode_p9(struct pt_regs *regs);

static int hv_mode;

static struct {
	u64	lpcr;
	u64	hfscr;
	u64	fscr;
} system_registers;

static void (*init_pmu_registers)(void);

static void cpufeatures_flush_tlb(void)
{
	unsigned long rb;
	unsigned int i, num_sets;

	/*
	 * This is a temporary measure to keep equivalent TLB flush as the
	 * cputable based setup code.
	 */
	switch (PVR_VER(mfspr(SPRN_PVR))) {
	case PVR_POWER8:
	case PVR_POWER8E:
	case PVR_POWER8NVL:
		num_sets = POWER8_TLB_SETS;
		break;
	case PVR_POWER9:
		num_sets = POWER9_TLB_SETS_HASH;
		break;
	default:
		num_sets = 1;
		pr_err("cpufeatures unknown CPU version for boot TLB flush\n");
		break;
	}

	asm volatile("ptesync" : : : "memory");
	rb = TLBIEL_INVAL_SET;
	for (i = 0; i < num_sets; i++) {
		asm volatile("tlbiel %0" : : "r" (rb));
		rb += 1 << TLBIEL_INVAL_SET_SHIFT;
	}
	asm volatile("ptesync" : : : "memory");
}

static void __restore_cpu_cpufeatures(void)
{
	/*
	 * LPCR is restored by the power on engine already. It can be changed
	 * after early init e.g., by radix enable, and we have no unified API
	 * for saving and restoring such SPRs.
	 *
	 * This ->restore hook should really be removed from idle and register
	 * restore moved directly into the idle restore code, because this code
	 * doesn't know how idle is implemented or what it needs restored here.
	 *
	 * The best we can do to accommodate secondary boot and idle restore
	 * for now is "or" LPCR with existing.
	 */

	mtspr(SPRN_LPCR, system_registers.lpcr | mfspr(SPRN_LPCR));
	if (hv_mode) {
		mtspr(SPRN_LPID, 0);
		mtspr(SPRN_HFSCR, system_registers.hfscr);
	}
	mtspr(SPRN_FSCR, system_registers.fscr);

	if (init_pmu_registers)
		init_pmu_registers();

	cpufeatures_flush_tlb();
}

static struct cpu_spec __initdata base_cpu_spec = {
	.cpu_name		= NULL,
	.cpu_features		= CPU_FTRS_BASE,
	.cpu_user_features	= COMMON_USER_BASE,
	.cpu_user_features2	= COMMON_USER2_BASE,
	.mmu_features		= 0,
	.icache_bsize		= 32, /* minimum block size, fixed by */
	.dcache_bsize		= 32, /* cache info init.             */
	.num_pmcs		= 0,
	.pmc_type		= PPC_PMC_DEFAULT,
	.oprofile_cpu_type	= NULL,
	.oprofile_type		= PPC_OPROFILE_INVALID,
	.cpu_setup		= NULL,
	.cpu_restore		= __restore_cpu_cpufeatures,
	.flush_tlb		= NULL,
	.machine_check_early	= NULL,
	.platform		= NULL,
};

void __init cpufeatures_setup_cpu(void)
{
	set_cur_cpu_spec(&base_cpu_spec);

	cur_cpu_spec->pvr_mask = -1;
	cur_cpu_spec->pvr_value = mfspr(SPRN_PVR);

	/* Initialize the base environment -- clear FSCR/HFSCR.  */
	hv_mode = !!(mfmsr() & MSR_HV);
	if (hv_mode) {
		/* CPU_FTR_HVMODE is used early in PACA setup */
		cur_cpu_spec->cpu_features |= CPU_FTR_HVMODE;
		mtspr(SPRN_HFSCR, 0);
	}
	mtspr(SPRN_FSCR, 0);

	/*
	 * LPCR does not get cleared, to match behaviour with secondaries
	 * in __restore_cpu_cpufeatures. Once the idle code is fixed, this
	 * could clear LPCR too.
	 */
}

static int __init feat_try_enable_unknown(struct dt_cpu_feature *f)
{
	if (f->hv_support == HV_SUPPORT_NONE) {
	} else if (f->hv_support & HV_SUPPORT_HFSCR) {
		u64 hfscr = mfspr(SPRN_HFSCR);
		hfscr |= 1UL << f->hfscr_bit_nr;
		mtspr(SPRN_HFSCR, hfscr);
	} else {
		/* Does not have a known recipe */
		return 0;
	}

	if (f->os_support == OS_SUPPORT_NONE) {
	} else if (f->os_support & OS_SUPPORT_FSCR) {
		u64 fscr = mfspr(SPRN_FSCR);
		fscr |= 1UL << f->fscr_bit_nr;
		mtspr(SPRN_FSCR, fscr);
	} else {
		/* Does not have a known recipe */
		return 0;
	}

	if ((f->usable_privilege & USABLE_PR) && (f->hwcap_bit_nr != -1)) {
		uint32_t word = f->hwcap_bit_nr / 32;
		uint32_t bit = f->hwcap_bit_nr % 32;

		if (word == 0)
			cur_cpu_spec->cpu_user_features |= 1U << bit;
		else if (word == 1)
			cur_cpu_spec->cpu_user_features2 |= 1U << bit;
		else
			pr_err("CPU feature: %s could not advertise to user (no hwcap bits)\n", f->name);
	}

	return 1;
}

static int __init feat_enable(struct dt_cpu_feature *f)
{
	if (f->hv_support != HV_SUPPORT_NONE) {
		if (f->hfscr_bit_nr != -1) {
			u64 hfscr = mfspr(SPRN_HFSCR);
			hfscr |= 1UL << f->hfscr_bit_nr;
			mtspr(SPRN_HFSCR, hfscr);
		}
	}

	if (f->os_support != OS_SUPPORT_NONE) {
		if (f->fscr_bit_nr != -1) {
			u64 fscr = mfspr(SPRN_FSCR);
			fscr |= 1UL << f->fscr_bit_nr;
			mtspr(SPRN_FSCR, fscr);
		}
	}

	if ((f->usable_privilege & USABLE_PR) && (f->hwcap_bit_nr != -1)) {
		uint32_t word = f->hwcap_bit_nr / 32;
		uint32_t bit = f->hwcap_bit_nr % 32;

		if (word == 0)
			cur_cpu_spec->cpu_user_features |= 1U << bit;
		else if (word == 1)
			cur_cpu_spec->cpu_user_features2 |= 1U << bit;
		else
			pr_err("CPU feature: %s could not advertise to user (no hwcap bits)\n", f->name);
	}

	return 1;
}

static int __init feat_disable(struct dt_cpu_feature *f)
{
	return 0;
}

static int __init feat_enable_hv(struct dt_cpu_feature *f)
{
	u64 lpcr;

	if (!hv_mode) {
		pr_err("CPU feature hypervisor present in device tree but HV mode not enabled in the CPU. Ignoring.\n");
		return 0;
	}

	mtspr(SPRN_LPID, 0);

	lpcr = mfspr(SPRN_LPCR);
	lpcr &=  ~LPCR_LPES0; /* HV external interrupts */
	mtspr(SPRN_LPCR, lpcr);

	cur_cpu_spec->cpu_features |= CPU_FTR_HVMODE;

	return 1;
}

static int __init feat_enable_le(struct dt_cpu_feature *f)
{
	cur_cpu_spec->cpu_user_features |= PPC_FEATURE_TRUE_LE;
	return 1;
}

static int __init feat_enable_smt(struct dt_cpu_feature *f)
{
	cur_cpu_spec->cpu_features |= CPU_FTR_SMT;
	cur_cpu_spec->cpu_user_features |= PPC_FEATURE_SMT;
	return 1;
}

static int __init feat_enable_idle_nap(struct dt_cpu_feature *f)
{
	u64 lpcr;

	/* Set PECE wakeup modes for ISA 207 */
	lpcr = mfspr(SPRN_LPCR);
	lpcr |=  LPCR_PECE0;
	lpcr |=  LPCR_PECE1;
	lpcr |=  LPCR_PECE2;
	mtspr(SPRN_LPCR, lpcr);

	return 1;
}

static int __init feat_enable_align_dsisr(struct dt_cpu_feature *f)
{
	cur_cpu_spec->cpu_features &= ~CPU_FTR_NODSISRALIGN;

	return 1;
}

static int __init feat_enable_idle_stop(struct dt_cpu_feature *f)
{
	u64 lpcr;

	/* Set PECE wakeup modes for ISAv3.0B */
	lpcr = mfspr(SPRN_LPCR);
	lpcr |=  LPCR_PECE0;
	lpcr |=  LPCR_PECE1;
	lpcr |=  LPCR_PECE2;
	mtspr(SPRN_LPCR, lpcr);

	return 1;
}

static int __init feat_enable_mmu_hash(struct dt_cpu_feature *f)
{
	u64 lpcr;

	lpcr = mfspr(SPRN_LPCR);
	lpcr &= ~LPCR_ISL;

	/* VRMASD */
	lpcr |= LPCR_VPM0;
	lpcr &= ~LPCR_VPM1;
	lpcr |= 0x10UL << LPCR_VRMASD_SH; /* L=1 LP=00 */
	mtspr(SPRN_LPCR, lpcr);

	cur_cpu_spec->mmu_features |= MMU_FTRS_HASH_BASE;
	cur_cpu_spec->cpu_user_features |= PPC_FEATURE_HAS_MMU;

	return 1;
}

static int __init feat_enable_mmu_hash_v3(struct dt_cpu_feature *f)
{
	u64 lpcr;

	lpcr = mfspr(SPRN_LPCR);
	lpcr &= ~LPCR_ISL;
	mtspr(SPRN_LPCR, lpcr);

	cur_cpu_spec->mmu_features |= MMU_FTRS_HASH_BASE;
	cur_cpu_spec->cpu_user_features |= PPC_FEATURE_HAS_MMU;

	return 1;
}


static int __init feat_enable_mmu_radix(struct dt_cpu_feature *f)
{
#ifdef CONFIG_PPC_RADIX_MMU
	cur_cpu_spec->mmu_features |= MMU_FTR_TYPE_RADIX;
	cur_cpu_spec->mmu_features |= MMU_FTRS_HASH_BASE;
	cur_cpu_spec->cpu_user_features |= PPC_FEATURE_HAS_MMU;

	return 1;
#endif
	return 0;
}

static int __init feat_enable_dscr(struct dt_cpu_feature *f)
{
	u64 lpcr;

	feat_enable(f);

	lpcr = mfspr(SPRN_LPCR);
	lpcr &= ~LPCR_DPFD;
	lpcr |=  (4UL << LPCR_DPFD_SH);
	mtspr(SPRN_LPCR, lpcr);

	return 1;
}

static void hfscr_pmu_enable(void)
{
	u64 hfscr = mfspr(SPRN_HFSCR);
	hfscr |= PPC_BIT(60);
	mtspr(SPRN_HFSCR, hfscr);
}

static void init_pmu_power8(void)
{
	if (hv_mode) {
		mtspr(SPRN_MMCRC, 0);
		mtspr(SPRN_MMCRH, 0);
	}

	mtspr(SPRN_MMCRA, 0);
	mtspr(SPRN_MMCR0, 0);
	mtspr(SPRN_MMCR1, 0);
	mtspr(SPRN_MMCR2, 0);
	mtspr(SPRN_MMCRS, 0);
}

static int __init feat_enable_mce_power8(struct dt_cpu_feature *f)
{
	cur_cpu_spec->platform = "power8";
	cur_cpu_spec->flush_tlb = __flush_tlb_power8;
	cur_cpu_spec->machine_check_early = __machine_check_early_realmode_p8;

	return 1;
}

static int __init feat_enable_pmu_power8(struct dt_cpu_feature *f)
{
	hfscr_pmu_enable();

	init_pmu_power8();
	init_pmu_registers = init_pmu_power8;

	cur_cpu_spec->cpu_features |= CPU_FTR_MMCRA;
	cur_cpu_spec->cpu_user_features |= PPC_FEATURE_PSERIES_PERFMON_COMPAT;
	if (pvr_version_is(PVR_POWER8E))
		cur_cpu_spec->cpu_features |= CPU_FTR_PMAO_BUG;

	cur_cpu_spec->num_pmcs		= 6;
	cur_cpu_spec->pmc_type		= PPC_PMC_IBM;
	cur_cpu_spec->oprofile_cpu_type	= "ppc64/power8";

	return 1;
}

static void init_pmu_power9(void)
{
	if (hv_mode)
		mtspr(SPRN_MMCRC, 0);

	mtspr(SPRN_MMCRA, 0);
	mtspr(SPRN_MMCR0, 0);
	mtspr(SPRN_MMCR1, 0);
	mtspr(SPRN_MMCR2, 0);
}

static int __init feat_enable_mce_power9(struct dt_cpu_feature *f)
{
	cur_cpu_spec->platform = "power9";
	cur_cpu_spec->flush_tlb = __flush_tlb_power9;
	cur_cpu_spec->machine_check_early = __machine_check_early_realmode_p9;

	return 1;
}

static int __init feat_enable_pmu_power9(struct dt_cpu_feature *f)
{
	hfscr_pmu_enable();

	init_pmu_power9();
	init_pmu_registers = init_pmu_power9;

	cur_cpu_spec->cpu_features |= CPU_FTR_MMCRA;
	cur_cpu_spec->cpu_user_features |= PPC_FEATURE_PSERIES_PERFMON_COMPAT;

	cur_cpu_spec->num_pmcs		= 6;
	cur_cpu_spec->pmc_type		= PPC_PMC_IBM;
	cur_cpu_spec->oprofile_cpu_type	= "ppc64/power9";

	return 1;
}

static int __init feat_enable_tm(struct dt_cpu_feature *f)
{
#ifdef CONFIG_PPC_TRANSACTIONAL_MEM
	feat_enable(f);
	cur_cpu_spec->cpu_user_features2 |= PPC_FEATURE2_HTM_NOSC;
	return 1;
#endif
	return 0;
}

static int __init feat_enable_fp(struct dt_cpu_feature *f)
{
	feat_enable(f);
	cur_cpu_spec->cpu_features &= ~CPU_FTR_FPU_UNAVAILABLE;

	return 1;
}

static int __init feat_enable_vector(struct dt_cpu_feature *f)
{
#ifdef CONFIG_ALTIVEC
	feat_enable(f);
	cur_cpu_spec->cpu_features |= CPU_FTR_ALTIVEC;
	cur_cpu_spec->cpu_features |= CPU_FTR_VMX_COPY;
	cur_cpu_spec->cpu_user_features |= PPC_FEATURE_HAS_ALTIVEC;

	return 1;
#endif
	return 0;
}

static int __init feat_enable_vsx(struct dt_cpu_feature *f)
{
#ifdef CONFIG_VSX
	feat_enable(f);
	cur_cpu_spec->cpu_features |= CPU_FTR_VSX;
	cur_cpu_spec->cpu_user_features |= PPC_FEATURE_HAS_VSX;

	return 1;
#endif
	return 0;
}

static int __init feat_enable_purr(struct dt_cpu_feature *f)
{
	cur_cpu_spec->cpu_features |= CPU_FTR_PURR | CPU_FTR_SPURR;

	return 1;
}

static int __init feat_enable_ebb(struct dt_cpu_feature *f)
{
	/*
	 * PPC_FEATURE2_EBB is enabled in PMU init code because it has
	 * historically been related to the PMU facility. This may have
	 * to be decoupled if EBB becomes more generic. For now, follow
	 * existing convention.
	 */
	f->hwcap_bit_nr = -1;
	feat_enable(f);

	return 1;
}

static int __init feat_enable_dbell(struct dt_cpu_feature *f)
{
	u64 lpcr;

	/* P9 has an HFSCR for privileged state */
	feat_enable(f);

	cur_cpu_spec->cpu_features |= CPU_FTR_DBELL;

	lpcr = mfspr(SPRN_LPCR);
	lpcr |=  LPCR_PECEDH; /* hyp doorbell wakeup */
	mtspr(SPRN_LPCR, lpcr);

	return 1;
}

static int __init feat_enable_hvi(struct dt_cpu_feature *f)
{
	u64 lpcr;

	/*
	 * POWER9 XIVE interrupts including in OPAL XICS compatibility
	 * are always delivered as hypervisor virtualization interrupts (HVI)
	 * rather than EE.
	 *
	 * However LPES0 is not set here, in the chance that an EE does get
	 * delivered to the host somehow, the EE handler would not expect it
	 * to be delivered in LPES0 mode (e.g., using SRR[01]). This could
	 * happen if there is a bug in interrupt controller code, or IC is
	 * misconfigured in systemsim.
	 */

	lpcr = mfspr(SPRN_LPCR);
	lpcr |= LPCR_HVICE;	/* enable hvi interrupts */
	lpcr |= LPCR_HEIC;	/* disable ee interrupts when MSR_HV */
	lpcr |= LPCR_PECE_HVEE; /* hvi can wake from stop */
	mtspr(SPRN_LPCR, lpcr);

	return 1;
}

static int __init feat_enable_large_ci(struct dt_cpu_feature *f)
{
	cur_cpu_spec->mmu_features |= MMU_FTR_CI_LARGE_PAGE;

	return 1;
}

struct dt_cpu_feature_match {
	const char *name;
	int (*enable)(struct dt_cpu_feature *f);
	u64 cpu_ftr_bit_mask;
};

static struct dt_cpu_feature_match __initdata
		dt_cpu_feature_match_table[] = {
	{"hypervisor", feat_enable_hv, 0},
	{"big-endian", feat_enable, 0},
	{"little-endian", feat_enable_le, CPU_FTR_REAL_LE},
	{"smt", feat_enable_smt, 0},
	{"interrupt-facilities", feat_enable, 0},
	{"timer-facilities", feat_enable, 0},
	{"timer-facilities-v3", feat_enable, 0},
	{"debug-facilities", feat_enable, 0},
	{"come-from-address-register", feat_enable, CPU_FTR_CFAR},
	{"branch-tracing", feat_enable, 0},
	{"floating-point", feat_enable_fp, 0},
	{"vector", feat_enable_vector, 0},
	{"vector-scalar", feat_enable_vsx, 0},
	{"vector-scalar-v3", feat_enable, 0},
	{"decimal-floating-point", feat_enable, 0},
	{"decimal-integer", feat_enable, 0},
	{"quadword-load-store", feat_enable, 0},
	{"vector-crypto", feat_enable, 0},
	{"mmu-hash", feat_enable_mmu_hash, 0},
	{"mmu-radix", feat_enable_mmu_radix, 0},
	{"mmu-hash-v3", feat_enable_mmu_hash_v3, 0},
	{"virtual-page-class-key-protection", feat_enable, 0},
	{"transactional-memory", feat_enable_tm, CPU_FTR_TM},
	{"transactional-memory-v3", feat_enable_tm, 0},
	{"idle-nap", feat_enable_idle_nap, 0},
	{"alignment-interrupt-dsisr", feat_enable_align_dsisr, 0},
	{"idle-stop", feat_enable_idle_stop, 0},
	{"machine-check-power8", feat_enable_mce_power8, 0},
	{"performance-monitor-power8", feat_enable_pmu_power8, 0},
	{"data-stream-control-register", feat_enable_dscr, CPU_FTR_DSCR},
	{"event-based-branch", feat_enable_ebb, 0},
	{"target-address-register", feat_enable, 0},
	{"branch-history-rolling-buffer", feat_enable, 0},
	{"control-register", feat_enable, CPU_FTR_CTRL},
	{"processor-control-facility", feat_enable_dbell, CPU_FTR_DBELL},
	{"processor-control-facility-v3", feat_enable_dbell, CPU_FTR_DBELL},
	{"processor-utilization-of-resources-register", feat_enable_purr, 0},
	{"subcore", feat_enable, CPU_FTR_SUBCORE},
	{"no-execute", feat_enable, 0},
	{"strong-access-ordering", feat_enable, CPU_FTR_SAO},
	{"cache-inhibited-large-page", feat_enable_large_ci, 0},
	{"coprocessor-icswx", feat_enable, CPU_FTR_ICSWX},
	{"hypervisor-virtualization-interrupt", feat_enable_hvi, 0},
	{"program-priority-register", feat_enable, CPU_FTR_HAS_PPR},
	{"wait", feat_enable, 0},
	{"atomic-memory-operations", feat_enable, 0},
	{"branch-v3", feat_enable, 0},
	{"copy-paste", feat_enable, 0},
	{"decimal-floating-point-v3", feat_enable, 0},
	{"decimal-integer-v3", feat_enable, 0},
	{"fixed-point-v3", feat_enable, 0},
	{"floating-point-v3", feat_enable, 0},
	{"group-start-register", feat_enable, 0},
	{"pc-relative-addressing", feat_enable, 0},
	{"machine-check-power9", feat_enable_mce_power9, 0},
	{"performance-monitor-power9", feat_enable_pmu_power9, 0},
	{"event-based-branch-v3", feat_enable, 0},
	{"random-number-generator", feat_enable, 0},
	{"system-call-vectored", feat_disable, 0},
	{"trace-interrupt-v3", feat_enable, 0},
	{"vector-v3", feat_enable, 0},
	{"vector-binary128", feat_enable, 0},
	{"vector-binary16", feat_enable, 0},
	{"wait-v3", feat_enable, 0},
};

/* XXX: how to configure this? Default + boot time? */
#ifdef CONFIG_PPC_CPUFEATURES_ENABLE_UNKNOWN
#define CPU_FEATURE_ENABLE_UNKNOWN 1
#else
#define CPU_FEATURE_ENABLE_UNKNOWN 0
#endif

void __init cpufeatures_setup_start(u32 isa)
{
	DBG("CPUFEATURES setup for isa %d\n", isa);

	if (isa >= 3000) {
		cur_cpu_spec->cpu_features |= CPU_FTR_ARCH_300;
		cur_cpu_spec->cpu_user_features2 |= PPC_FEATURE2_ARCH_3_00;
	}
}

int __init cpufeatures_process_feature(struct dt_cpu_feature *f)
{
	const struct dt_cpu_feature_match *m;
	int known = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(dt_cpu_feature_match_table); i++) {
		m = &dt_cpu_feature_match_table[i];
		if (!strcmp(f->name, m->name)) {
			known = 1;
			if (m->enable(f))
				goto enabled;
			goto not_enabled;
		}
	}

	if (CPU_FEATURE_ENABLE_UNKNOWN) {
		if (feat_try_enable_unknown(f))
			goto enabled;
	}

not_enabled:
	if (known)
		DBG("CPU feature not enabling:%s (disabled or unsupported by kernel)\n", f->name);
	else
		DBG("CPU feature not enabling:%s (unknown and unsupported by kernel)\n", f->name);

	return 0;

enabled:
	if (m->cpu_ftr_bit_mask)
		cur_cpu_spec->cpu_features |= m->cpu_ftr_bit_mask;
	if (known)
		DBG("CPU feature enabling:%s\n", f->name);
	else
		DBG("CPU feature enabling:%s (unknown)\n", f->name);

	return 1;
}

static __init void cpufeatures_cpu_quirks(void)
{
	int version = mfspr(SPRN_PVR);

	/*
	 * Not all quirks can be derived from the cpufeatures device tree.
	 */
	if ((version & 0xffffff00) == 0x004e0100)
		cur_cpu_spec->cpu_features |= CPU_FTR_POWER9_DD1;
}

void __init cpufeatures_setup_finished(void)
{
	cpufeatures_cpu_quirks();

	if (hv_mode && !(cur_cpu_spec->cpu_features & CPU_FTR_HVMODE)) {
		pr_err("CPU feature hypervisor not present in device tree but HV mode is enabled in the CPU. Enabling.\n");
		cur_cpu_spec->cpu_features |= CPU_FTR_HVMODE;
	}

	system_registers.lpcr = mfspr(SPRN_LPCR);
	system_registers.hfscr = mfspr(SPRN_HFSCR);
	system_registers.fscr = mfspr(SPRN_FSCR);

	cpufeatures_flush_tlb();
}
