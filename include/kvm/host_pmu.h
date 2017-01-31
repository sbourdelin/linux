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
#ifndef _KVM_HOST_PMU_H
#define _KVM_HOST_PMU_H

#include <linux/kvm_host.h>
#include <linux/perf_event.h>

typedef void (*configure_event_fn)(struct kvm_vcpu *vcpu, bool enable);
typedef u64 (*get_event_count_fn)(struct kvm_vcpu *vcpu);

struct kvm_event_cb {
	get_event_count_fn get_event_count;
	configure_event_fn configure_event;
};

int kvm_host_pmu_register(int nr_events, struct kvm_event_cb *cbs,
			  struct attribute **event_attrs);
void kvm_host_pmu_unregister(void);

#endif /* _KVM_HOST_PMU_H */
