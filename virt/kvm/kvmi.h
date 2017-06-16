/*
 * Copyright (C) 2017 Bitdefender S.R.L.
 *
 * The KVMI Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * The KVMI Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the GNU C Library; if not, see
 * <http://www.gnu.org/licenses/>
 */
#ifndef __KVMI_H__
#define __KVMI_H__

#include <linux/kvm_host.h>

int kvmi_init(void);
void kvmi_uninit(void);
void kvmi_vm_powered_on(struct kvm *kvm);
void kvmi_vm_powered_off(struct kvm *kvm);
bool kvmi_cr_event(struct kvm_vcpu *vcpu, unsigned int cr,
		   unsigned long old_value, unsigned long *new_value);
bool kvmi_msr_event(struct kvm_vcpu *vcpu, unsigned int msr,
		    u64 old_value, u64 *new_value);
void kvmi_xsetbv_event(struct kvm_vcpu *vcpu, u64 value);
bool kvmi_breakpoint_event(struct kvm_vcpu *vcpu, u64 gpa);
void kvmi_vmcall_event(struct kvm_vcpu *vcpu);
bool kvmi_page_fault(struct kvm_vcpu *vcpu, unsigned long gpa,
		     unsigned long gva, unsigned int mode, unsigned int *opts);
void kvmi_trap_event(struct kvm_vcpu *vcpu, unsigned int vector,
		     unsigned int type, unsigned int err, u64 cr2);
void kvmi_flush_mem_access(struct kvm_vcpu *vcpu);
void kvmi_handle_controller_request(struct kvm_vcpu *vcpu);
int kvmi_patch_emul_instr(struct kvm_vcpu *vcpu, void *val, unsigned int bytes);

#endif
