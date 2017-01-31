/*
 * Copyright (C) 2017 ARM Ltd.
 * Author: Punit Agrawal <punit.agrawal@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <kvm/host_pmu.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/kvm_host.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include <asm/kvm_emulate.h>

enum host_pmu_events {
	tlb_invalidate,
	MAX_EVENTS,
};

static u64 get_tlb_invalidate_count(struct kvm_vcpu *vcpu)
{
	return vcpu->stat.tlb_invalidate;
}

static void configure_tlb_invalidate(struct kvm_vcpu *vcpu, bool enable)
{
	unsigned long hcr;

	hcr = vcpu_get_hcr(vcpu);

	if (enable)
		hcr |= HCR_TTLB;
	else
		hcr &= ~HCR_TTLB;

	vcpu_set_hcr(vcpu, hcr);
}

static ssize_t events_sysfs_show(struct device *dev,
				 struct device_attribute *attr, char *page)
{
	struct perf_pmu_events_attr *pmu_attr;

	pmu_attr = container_of(attr, struct perf_pmu_events_attr, attr);

	return sprintf(page, "event=0x%03llx\n", pmu_attr->id);
}
PMU_EVENT_ATTR(tlb_invalidate, event_attr_tlb_invalidate, tlb_invalidate,
	       events_sysfs_show);

static struct attribute *event_attrs[] = {
	&event_attr_tlb_invalidate.attr.attr,
	NULL,
};

static struct kvm_event_cb event_callbacks[] = {
	{
		.get_event_count = get_tlb_invalidate_count,
		.configure_event = configure_tlb_invalidate,
	},
};

int arm_host_pmu_init(void)
{
	return kvm_host_pmu_register(MAX_EVENTS, event_callbacks, event_attrs);
}

void arm_host_pmu_teardown(void)
{
	kvm_host_pmu_unregister();
}
