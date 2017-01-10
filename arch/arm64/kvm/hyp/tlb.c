/*
 * Copyright (C) 2015 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
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

#include <asm/kvm_hyp.h>
#include <asm/tlbflush.h>

void __hyp_text __kvm_tlb_flush_vmid_ipa(struct kvm *kvm, phys_addr_t ipa)
{
	dsb(ishst);

	/* Switch to requested VMID */
	kvm = kern_hyp_va(kvm);
	write_sysreg(kvm->arch.vttbr, vttbr_el2);
	isb();

	/*
	 * We could do so much better if we had the VA as well.
	 * Instead, we invalidate Stage-2 for this IPA, and the
	 * whole of Stage-1. Weep...
	 */
	ipa >>= 12;
	__tlbi(ipas2e1is, ipa);

	/*
	 * We have to ensure completion of the invalidation at Stage-2,
	 * since a table walk on another CPU could refill a TLB with a
	 * complete (S1 + S2) walk based on the old Stage-2 mapping if
	 * the Stage-1 invalidation happened first.
	 */
	dsb(ish);
	__tlbi(vmalle1is);
	dsb(ish);
	isb();

	write_sysreg(0, vttbr_el2);
}

void __hyp_text __kvm_tlb_flush_vmid(struct kvm *kvm)
{
	dsb(ishst);

	/* Switch to requested VMID */
	kvm = kern_hyp_va(kvm);
	write_sysreg(kvm->arch.vttbr, vttbr_el2);
	isb();

	__tlbi(vmalls12e1is);
	dsb(ish);
	isb();

	write_sysreg(0, vttbr_el2);
}

void __hyp_text __kvm_tlb_flush_local_vmid(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = kern_hyp_va(kern_hyp_va(vcpu)->kvm);

	/* Switch to requested VMID */
	write_sysreg(kvm->arch.vttbr, vttbr_el2);
	isb();

	asm volatile("tlbi vmalle1" : : );
	dsb(nsh);
	isb();

	write_sysreg(0, vttbr_el2);
}

void __hyp_text __kvm_flush_vm_context(void)
{
	dsb(ishst);
	__tlbi(alle1is);
	__flush_icache_all(); /* contains a dsb(ish) */
}

/* Intentionally empty functions */
static void __hyp_text __switch_to_hyp_role_nvhe(void) { }
static void __hyp_text __switch_to_host_role_nvhe(void) { }

static void __hyp_text __switch_to_hyp_role_vhe(void)
{
	u64 hcr = read_sysreg(hcr_el2);

	/*
	 * When VHE is enabled and HCR_EL2.TGE=1, EL1&0 TLB operations
	 * apply to EL2&0 translation regime. As we prepare to emulate
	 * guest TLB operation clear HCR_TGE to target TLB operations
	 * to EL1&0 (guest).
	 */
	hcr &= ~HCR_TGE;
	write_sysreg(hcr, hcr_el2);
}

static void __hyp_text __switch_to_host_role_vhe(void)
{
	u64 hcr = read_sysreg(hcr_el2);

	hcr |= HCR_TGE;
	write_sysreg(hcr, hcr_el2);
}

static hyp_alternate_select(__switch_to_hyp_role,
			    __switch_to_hyp_role_nvhe,
			    __switch_to_hyp_role_vhe,
			    ARM64_HAS_VIRT_HOST_EXTN);

static hyp_alternate_select(__switch_to_host_role,
			    __switch_to_host_role_nvhe,
			    __switch_to_host_role_vhe,
			    ARM64_HAS_VIRT_HOST_EXTN);

static void __hyp_text __switch_to_guest_regime(struct kvm *kvm)
{
	write_sysreg(kvm->arch.vttbr, vttbr_el2);
	__switch_to_hyp_role();
	isb();
}

static void __hyp_text __switch_to_host_regime(void)
{
	__switch_to_host_role();
	write_sysreg(0, vttbr_el2);
}

void __hyp_text
__kvm_emulate_tlb_invalidate(struct kvm *kvm, u32 opcode, u64 regval)
{
	kvm = kern_hyp_va(kvm);

	/*
	 * Switch to the guest before performing any TLB operations to
	 * target the appropriate VMID
	 */
	__switch_to_guest_regime(kvm);

	/*
	 *  TLB maintenance operations are broadcast to
	 *  inner-shareable domain when HCR_FB is set (default for
	 *  KVM).
	 *
	 *  Nuke all Stage 1 TLB entries for the VM. This will kill
	 *  performance but it's always safe to do as we don't leave
	 *  behind any strays in the TLB
	 */
	__tlbi(vmalle1is);
	isb();

	__switch_to_host_regime();
}
