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

static void __hyp_text __tlb_flush_vmid_ipa(struct kvm *kvm, phys_addr_t ipa)
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

__alias(__tlb_flush_vmid_ipa) void __kvm_tlb_flush_vmid_ipa(struct kvm *kvm,
							    phys_addr_t ipa);

static void __hyp_text __tlb_flush_vmid(struct kvm *kvm)
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

__alias(__tlb_flush_vmid) void __kvm_tlb_flush_vmid(struct kvm *kvm);

static void __hyp_text __tlb_flush_vm_context(void)
{
	dsb(ishst);
	__tlbi(alle1is);
	__flush_icache_all(); /* contains a dsb(ish) */
}

__alias(__tlb_flush_vm_context) void __kvm_flush_vm_context(void);

/* Intentionally empty functions */
static void __hyp_text __switch_to_hyp_role_nvhe(void) { }
static void __hyp_text __switch_to_host_role_nvhe(void) { }

static void __hyp_text __switch_to_hyp_role_vhe(void)
{
	u64 hcr = read_sysreg(hcr_el2);

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

/*
 *  AArch32 TLB maintenance instructions trapping to EL2
 */
#define TLBIALLIS			sys_reg(0, 0, 8, 3, 0)
#define TLBIMVAIS			sys_reg(0, 0, 8, 3, 1)
#define TLBIASIDIS			sys_reg(0, 0, 8, 3, 2)
#define TLBIMVAAIS			sys_reg(0, 0, 8, 3, 3)
#define TLBIMVALIS			sys_reg(0, 0, 8, 3, 5)
#define TLBIMVAALIS			sys_reg(0, 0, 8, 3, 7)
#define ITLBIALL			sys_reg(0, 0, 8, 5, 0)
#define ITLBIMVA			sys_reg(0, 0, 8, 5, 1)
#define ITLBIASID			sys_reg(0, 0, 8, 5, 2)
#define DTLBIALL			sys_reg(0, 0, 8, 6, 0)
#define DTLBIMVA			sys_reg(0, 0, 8, 6, 1)
#define DTLBIASID			sys_reg(0, 0, 8, 6, 2)
#define TLBIALL				sys_reg(0, 0, 8, 7, 0)
#define TLBIMVA				sys_reg(0, 0, 8, 7, 1)
#define TLBIASID			sys_reg(0, 0, 8, 7, 2)
#define TLBIMVAA			sys_reg(0, 0, 8, 7, 3)
#define TLBIMVAL			sys_reg(0, 0, 8, 7, 5)
#define TLBIMVAAL			sys_reg(0, 0, 8, 7, 7)

/*
 * ARMv8 ARM: Table C5-4 TLB maintenance instructions
 * (Ref: ARMv8 ARM C5.1 version: ARM DDI 0487A.j)
 */
#define TLBI_VMALLE1IS			sys_reg(1, 0, 8, 3, 0)
#define TLBI_VAE1IS			sys_reg(1, 0, 8, 3, 1)
#define TLBI_ASIDE1IS			sys_reg(1, 0, 8, 3, 2)
#define TLBI_VAAE1IS			sys_reg(1, 0, 8, 3, 3)
#define TLBI_VALE1IS			sys_reg(1, 0, 8, 3, 5)
#define TLBI_VAALE1IS			sys_reg(1, 0, 8, 3, 7)
#define TLBI_VMALLE1			sys_reg(1, 0, 8, 7, 0)
#define TLBI_VAE1			sys_reg(1, 0, 8, 7, 1)
#define TLBI_ASIDE1			sys_reg(1, 0, 8, 7, 2)
#define TLBI_VAAE1			sys_reg(1, 0, 8, 7, 3)
#define TLBI_VALE1			sys_reg(1, 0, 8, 7, 5)
#define TLBI_VAALE1			sys_reg(1, 0, 8, 7, 7)

void __hyp_text
__kvm_emulate_tlb_invalidate(struct kvm *kvm, u32 sys_op, u64 regval)
{
	kvm = kern_hyp_va(kvm);

	/*
	 * Switch to the guest before performing any TLB operations to
	 * target the appropriate VMID
	 */
	__switch_to_guest_regime(kvm);

	/*
	 *  TLB maintenance operations broadcast to inner-shareable
	 *  domain when HCR_FB is set (default for KVM).
	 */
	switch (sys_op) {
	case TLBIALL:
	case TLBIALLIS:
	case ITLBIALL:
	case DTLBIALL:
	case TLBI_VMALLE1:
	case TLBI_VMALLE1IS:
		__tlbi(vmalle1is);
		break;
	case TLBIMVA:
	case TLBIMVAIS:
	case ITLBIMVA:
	case DTLBIMVA:
	case TLBI_VAE1:
	case TLBI_VAE1IS:
		__tlbi(vae1is, regval);
		break;
	case TLBIASID:
	case TLBIASIDIS:
	case ITLBIASID:
	case DTLBIASID:
	case TLBI_ASIDE1:
	case TLBI_ASIDE1IS:
		__tlbi(aside1is, regval);
		break;
	case TLBIMVAA:
	case TLBIMVAAIS:
	case TLBI_VAAE1:
	case TLBI_VAAE1IS:
		__tlbi(vaae1is, regval);
		break;
	case TLBIMVAL:
	case TLBIMVALIS:
	case TLBI_VALE1:
	case TLBI_VALE1IS:
		__tlbi(vale1is, regval);
		break;
	case TLBIMVAAL:
	case TLBIMVAALIS:
	case TLBI_VAALE1:
	case TLBI_VAALE1IS:
		__tlbi(vaale1is, regval);
		break;
	}
	isb();

	__switch_to_host_regime();
}
