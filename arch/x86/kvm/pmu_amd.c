/*
 * KVM PMU support for AMD
 *
 * Copyright 2015, Red Hat, Inc. and/or its affiliates.
 *
 * Author:
 *   Wei Huang <wei@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Implementation is based on pmu_intel.c file
 */
#include <linux/types.h>
#include <linux/kvm_host.h>
#include <linux/perf_event.h>
#include "x86.h"
#include "cpuid.h"
#include "lapic.h"
#include "pmu.h"

enum pmu_type {
	PMU_TYPE_COUNTER = 0,
	PMU_TYPE_EVNTSEL,
};

/* duplicated from amd_perfmon_event_map, K7 and above should work. */
static struct kvm_event_hw_type_mapping amd_event_mapping[] = {
	[0] = { 0x76, 0x00, PERF_COUNT_HW_CPU_CYCLES },
	[1] = { 0xc0, 0x00, PERF_COUNT_HW_INSTRUCTIONS },
	[2] = { 0x7d, 0x07, PERF_COUNT_HW_CACHE_REFERENCES },
	[3] = { 0x7e, 0x07, PERF_COUNT_HW_CACHE_MISSES },
	[4] = { 0xc2, 0x00, PERF_COUNT_HW_BRANCH_INSTRUCTIONS },
	[5] = { 0xc3, 0x00, PERF_COUNT_HW_BRANCH_MISSES },
	[6] = { 0xd0, 0x00, PERF_COUNT_HW_STALLED_CYCLES_FRONTEND },
	[7] = { 0xd1, 0x00, PERF_COUNT_HW_STALLED_CYCLES_BACKEND },
};

static unsigned int get_msr_base(struct kvm_pmu *pmu, enum pmu_type type)
{
	struct kvm_vcpu *vcpu = pmu_to_vcpu(pmu);
	int family;
	bool has_perf_ext;

	family = guest_cpuid_family(vcpu);
	has_perf_ext = guest_cpuid_has(vcpu, X86_FEATURE_PERFCTR_CORE);

	switch (family) {
	case 0x15:
	case 0x17:
		if (has_perf_ext) {
			if (type == PMU_TYPE_COUNTER)
				return MSR_F15H_PERF_CTR;
			else
				return MSR_F15H_PERF_CTL;
			break;
		}
		/*
		 * Fall-through because the K7 MSRs are
		 * backwards compatible
		 */
	default:
		if (type == PMU_TYPE_COUNTER)
			return MSR_K7_PERFCTR0;
		else
			return MSR_K7_EVNTSEL0;
	}
}

static inline struct kvm_pmc *get_gp_pmc_amd(struct kvm_pmu *pmu, u32 msr,
					     enum pmu_type type)
{
	unsigned int base = get_msr_base(pmu, type);

	if (base == MSR_F15H_PERF_CTL) {
		switch (msr) {
		case MSR_F15H_PERF_CTL0:
		case MSR_F15H_PERF_CTL1:
		case MSR_F15H_PERF_CTL2:
		case MSR_F15H_PERF_CTL3:
		case MSR_F15H_PERF_CTL4:
		case MSR_F15H_PERF_CTL5:
			/*
			 * AMD Perf Extension MSRs are not continuous.
			 *
			 * E.g. MSR_F15H_PERF_CTR0 -> 0xc0010201
			 *	MSR_F15H_PERF_CTR1 -> 0xc0010203
			 *
			 * These are mapped to work with gp_counters[].
			 * The index into the array is calculated by
			 * dividing the difference between the requested
			 * msr and the msr base by 2.
			 *
			 * E.g. MSR_F15H_PERF_CTR1 uses
			 *	->gp_counters[(0xc0010203-0xc0010201)/2]
			 *	->gp_counters[1]
			 */
			return &pmu->gp_counters[(msr - base) >> 1];
		default:
			return NULL;
		}
	} else if (base == MSR_F15H_PERF_CTR) {
		switch (msr) {
		case MSR_F15H_PERF_CTR0:
		case MSR_F15H_PERF_CTR1:
		case MSR_F15H_PERF_CTR2:
		case MSR_F15H_PERF_CTR3:
		case MSR_F15H_PERF_CTR4:
		case MSR_F15H_PERF_CTR5:
			return &pmu->gp_counters[(msr - base) >> 1];
		default:
			return NULL;
		}
	} else {
		return get_gp_pmc(pmu, msr, base);
	}
}

static unsigned amd_find_arch_event(struct kvm_pmu *pmu,
				    u8 event_select,
				    u8 unit_mask)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(amd_event_mapping); i++)
		if (amd_event_mapping[i].eventsel == event_select
		    && amd_event_mapping[i].unit_mask == unit_mask)
			break;

	if (i == ARRAY_SIZE(amd_event_mapping))
		return PERF_COUNT_HW_MAX;

	return amd_event_mapping[i].event_type;
}

/* return PERF_COUNT_HW_MAX as AMD doesn't have fixed events */
static unsigned amd_find_fixed_event(int idx)
{
	return PERF_COUNT_HW_MAX;
}

/* check if a PMC is enabled by comparing it against global_ctrl bits. Because
 * AMD CPU doesn't have global_ctrl MSR, all PMCs are enabled (return TRUE).
 */
static bool amd_pmc_is_enabled(struct kvm_pmc *pmc)
{
	return true;
}

static struct kvm_pmc *amd_pmc_idx_to_pmc(struct kvm_pmu *pmu, int pmc_idx)
{
	unsigned int base = get_msr_base(pmu, PMU_TYPE_COUNTER);
	unsigned int family;

	family = guest_cpuid_family(pmu_to_vcpu(pmu));

	if (family == 0x15 || family == 0x17) {
		/*
		 * The idx is contiguous. The MSRs are not. The counter MSRs
		 * are interleaved with the event select MSRs.
		 */
		pmc_idx *= 2;
	}

	return get_gp_pmc_amd(pmu, base + pmc_idx, PMU_TYPE_COUNTER);
}

/* returns 0 if idx's corresponding MSR exists; otherwise returns 1. */
static int amd_is_valid_msr_idx(struct kvm_vcpu *vcpu, unsigned idx)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);

	idx &= ~(3u << 30);

	return (idx >= pmu->nr_arch_gp_counters);
}

/* idx is the ECX register of RDPMC instruction */
static struct kvm_pmc *amd_msr_idx_to_pmc(struct kvm_vcpu *vcpu, unsigned idx)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *counters;

	idx &= ~(3u << 30);
	if (idx >= pmu->nr_arch_gp_counters)
		return NULL;
	counters = pmu->gp_counters;

	return &counters[idx];
}

static bool amd_is_valid_msr(struct kvm_vcpu *vcpu, u32 msr)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	int ret = false;

	ret = get_gp_pmc_amd(pmu, msr, PMU_TYPE_COUNTER) ||
		get_gp_pmc_amd(pmu, msr, PMU_TYPE_EVNTSEL);

	return ret;
}

static int amd_pmu_get_msr(struct kvm_vcpu *vcpu, u32 msr, u64 *data)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *pmc;

	/* MSR_PERFCTRn */
	pmc = get_gp_pmc_amd(pmu, msr, PMU_TYPE_COUNTER);
	if (pmc) {
		*data = pmc_read_counter(pmc);
		return 0;
	}
	/* MSR_EVNTSELn */
	pmc = get_gp_pmc_amd(pmu, msr, PMU_TYPE_EVNTSEL);
	if (pmc) {
		*data = pmc->eventsel;
		return 0;
	}

	return 1;
}

static int amd_pmu_set_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *pmc;
	u32 msr = msr_info->index;
	u64 data = msr_info->data;

	/* MSR_PERFCTRn */
	pmc = get_gp_pmc_amd(pmu, msr, PMU_TYPE_COUNTER);
	if (pmc) {
		pmc->counter += data - pmc_read_counter(pmc);
		return 0;
	}
	/* MSR_EVNTSELn */
	pmc = get_gp_pmc_amd(pmu, msr, PMU_TYPE_EVNTSEL);
	if (pmc) {
		if (data == pmc->eventsel)
			return 0;
		if (!(data & pmu->reserved_bits)) {
			reprogram_gp_counter(pmc, data);
			return 0;
		}
	}

	return 1;
}

static void amd_pmu_refresh(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	int family, nr_counters;

	family = guest_cpuid_family(vcpu);
	if (family == 0x15 || family == 0x17)
		nr_counters = AMD64_NUM_COUNTERS_CORE;
	else
		nr_counters = AMD64_NUM_COUNTERS;

	pmu->nr_arch_gp_counters = nr_counters;
	pmu->counter_bitmask[KVM_PMC_GP] = ((u64)1 << 48) - 1;
	pmu->reserved_bits = 0xffffffff00200000ull;
	/* not applicable to AMD; but clean them to prevent any fall out */
	pmu->counter_bitmask[KVM_PMC_FIXED] = 0;
	pmu->nr_arch_fixed_counters = 0;
	pmu->version = 0;
	pmu->global_status = 0;
}

static void amd_pmu_init(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	int i;

	for (i = 0; i < AMD64_NUM_COUNTERS_CORE ; i++) {
		pmu->gp_counters[i].type = KVM_PMC_GP;
		pmu->gp_counters[i].vcpu = vcpu;
		pmu->gp_counters[i].idx = i;
	}
}

static void amd_pmu_reset(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	int i;

	for (i = 0; i < AMD64_NUM_COUNTERS_CORE; i++) {
		struct kvm_pmc *pmc = &pmu->gp_counters[i];

		pmc_stop_counter(pmc);
		pmc->counter = pmc->eventsel = 0;
	}
}

struct kvm_pmu_ops amd_pmu_ops = {
	.find_arch_event = amd_find_arch_event,
	.find_fixed_event = amd_find_fixed_event,
	.pmc_is_enabled = amd_pmc_is_enabled,
	.pmc_idx_to_pmc = amd_pmc_idx_to_pmc,
	.msr_idx_to_pmc = amd_msr_idx_to_pmc,
	.is_valid_msr_idx = amd_is_valid_msr_idx,
	.is_valid_msr = amd_is_valid_msr,
	.get_msr = amd_pmu_get_msr,
	.set_msr = amd_pmu_set_msr,
	.refresh = amd_pmu_refresh,
	.init = amd_pmu_init,
	.reset = amd_pmu_reset,
};
