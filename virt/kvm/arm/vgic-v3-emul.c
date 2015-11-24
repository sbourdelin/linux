/*
 * GICv3 distributor and redistributor emulation
 *
 * GICv3 emulation is currently only supported on a GICv3 host (because
 * we rely on the hardware's CPU interface virtualization support), but
 * supports both hardware with or without the optional GICv2 backwards
 * compatibility features.
 *
 * Limitations of the emulation:
 * (RAZ/WI: read as zero, write ignore, RAO/WI: read as one, write ignore)
 * - We do not support LPIs (yet). TYPER.LPIS is reported as 0 and is RAZ/WI.
 * - We do not support the message based interrupts (MBIs) triggered by
 *   writes to the GICD_{SET,CLR}SPI_* registers. TYPER.MBIS is reported as 0.
 * - We do not support the (optional) backwards compatibility feature.
 *   GICD_CTLR.ARE resets to 1 and is RAO/WI. If the _host_ GIC supports
 *   the compatiblity feature, you can use a GICv2 in the guest, though.
 * - We only support a single security state. GICD_CTLR.DS is 1 and is RAO/WI.
 * - Priorities are not emulated (same as the GICv2 emulation). Linux
 *   as a guest is fine with this, because it does not use priorities.
 * - We only support Group1 interrupts. Again Linux uses only those.
 *
 * Copyright (C) 2014 ARM Ltd.
 * Author: Andre Przywara <andre.przywara@arm.com>
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

#include <linux/cpu.h>
#include <linux/kvm.h>
#include <linux/kvm_host.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>

#include <linux/irqchip/arm-gic-v3.h>
#include <kvm/arm_vgic.h>

#include <asm/kvm_emulate.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_mmu.h>

#include "sys_regs.h"
#include "vgic.h"

static bool handle_mmio_rao_wi(struct kvm_vcpu *vcpu,
			       struct kvm_exit_mmio *mmio, phys_addr_t offset)
{
	u32 reg = 0xffffffff;

	vgic_reg_access(mmio, &reg, offset,
			ACCESS_READ_VALUE | ACCESS_WRITE_IGNORED);

	return false;
}

static bool handle_mmio_ctlr(struct kvm_vcpu *vcpu,
			     struct kvm_exit_mmio *mmio, phys_addr_t offset)
{
	u32 reg = 0;

	/*
	 * Force ARE and DS to 1, the guest cannot change this.
	 * For the time being we only support Group1 interrupts.
	 */
	if (vcpu->kvm->arch.vgic.enabled)
		reg = GICD_CTLR_ENABLE_SS_G1;
	reg |= GICD_CTLR_ARE_NS | GICD_CTLR_DS;

	vgic_reg_access(mmio, &reg, offset,
			ACCESS_READ_VALUE | ACCESS_WRITE_VALUE);
	if (mmio->is_write) {
		vcpu->kvm->arch.vgic.enabled = !!(reg & GICD_CTLR_ENABLE_SS_G1);
		vgic_update_state(vcpu->kvm);
		return true;
	}
	return false;
}

/*
 * As this implementation does not provide compatibility
 * with GICv2 (ARE==1), we report zero CPUs in bits [5..7].
 * Also LPIs and MBIs are not supported, so we set the respective bits to 0.
 * Also we report at most 2**10=1024 interrupt IDs (to match 1024 SPIs).
 */
#define INTERRUPT_ID_BITS 10
static bool handle_mmio_typer(struct kvm_vcpu *vcpu,
			      struct kvm_exit_mmio *mmio, phys_addr_t offset)
{
	u32 reg;

	reg = (min(vcpu->kvm->arch.vgic.nr_irqs, 1024) >> 5) - 1;

	reg |= (INTERRUPT_ID_BITS - 1) << 19;

	vgic_reg_access(mmio, &reg, offset,
			ACCESS_READ_VALUE | ACCESS_WRITE_IGNORED);

	return false;
}

static bool handle_mmio_iidr(struct kvm_vcpu *vcpu,
			     struct kvm_exit_mmio *mmio, phys_addr_t offset)
{
	u32 reg;

	reg = (PRODUCT_ID_KVM << 24) | (IMPLEMENTER_ARM << 0);
	vgic_reg_access(mmio, &reg, offset,
			ACCESS_READ_VALUE | ACCESS_WRITE_IGNORED);

	return false;
}

static bool handle_mmio_set_enable_reg_dist(struct kvm_vcpu *vcpu,
					    struct kvm_exit_mmio *mmio,
					    phys_addr_t offset)
{
	if (likely(offset >= VGIC_NR_PRIVATE_IRQS / 8))
		return vgic_handle_enable_reg(vcpu->kvm, mmio, offset,
					      vcpu->vcpu_id,
					      ACCESS_WRITE_SETBIT);

	vgic_reg_access(mmio, NULL, offset,
			ACCESS_READ_RAZ | ACCESS_WRITE_IGNORED);
	return false;
}

static bool handle_mmio_clear_enable_reg_dist(struct kvm_vcpu *vcpu,
					      struct kvm_exit_mmio *mmio,
					      phys_addr_t offset)
{
	if (likely(offset >= VGIC_NR_PRIVATE_IRQS / 8))
		return vgic_handle_enable_reg(vcpu->kvm, mmio, offset,
					      vcpu->vcpu_id,
					      ACCESS_WRITE_CLEARBIT);

	vgic_reg_access(mmio, NULL, offset,
			ACCESS_READ_RAZ | ACCESS_WRITE_IGNORED);
	return false;
}

static bool handle_mmio_set_pending_reg_dist(struct kvm_vcpu *vcpu,
					     struct kvm_exit_mmio *mmio,
					     phys_addr_t offset)
{
	if (likely(offset >= VGIC_NR_PRIVATE_IRQS / 8))
		return vgic_handle_set_pending_reg(vcpu->kvm, mmio, offset,
						   vcpu->vcpu_id);

	vgic_reg_access(mmio, NULL, offset,
			ACCESS_READ_RAZ | ACCESS_WRITE_IGNORED);
	return false;
}

static bool handle_mmio_clear_pending_reg_dist(struct kvm_vcpu *vcpu,
					       struct kvm_exit_mmio *mmio,
					       phys_addr_t offset)
{
	if (likely(offset >= VGIC_NR_PRIVATE_IRQS / 8))
		return vgic_handle_clear_pending_reg(vcpu->kvm, mmio, offset,
						     vcpu->vcpu_id);

	vgic_reg_access(mmio, NULL, offset,
			ACCESS_READ_RAZ | ACCESS_WRITE_IGNORED);
	return false;
}

static bool handle_mmio_set_active_reg_dist(struct kvm_vcpu *vcpu,
					    struct kvm_exit_mmio *mmio,
					    phys_addr_t offset)
{
	if (likely(offset >= VGIC_NR_PRIVATE_IRQS / 8))
		return vgic_handle_set_active_reg(vcpu->kvm, mmio, offset,
						   vcpu->vcpu_id);

	vgic_reg_access(mmio, NULL, offset,
			ACCESS_READ_RAZ | ACCESS_WRITE_IGNORED);
	return false;
}

static bool handle_mmio_clear_active_reg_dist(struct kvm_vcpu *vcpu,
					      struct kvm_exit_mmio *mmio,
					      phys_addr_t offset)
{
	if (likely(offset >= VGIC_NR_PRIVATE_IRQS / 8))
		return vgic_handle_clear_active_reg(vcpu->kvm, mmio, offset,
						    vcpu->vcpu_id);

	vgic_reg_access(mmio, NULL, offset,
			ACCESS_READ_RAZ | ACCESS_WRITE_IGNORED);
	return false;
}

static bool handle_mmio_priority_reg_dist(struct kvm_vcpu *vcpu,
					  struct kvm_exit_mmio *mmio,
					  phys_addr_t offset)
{
	u32 *reg;

	if (unlikely(offset < VGIC_NR_PRIVATE_IRQS)) {
		vgic_reg_access(mmio, NULL, offset,
				ACCESS_READ_RAZ | ACCESS_WRITE_IGNORED);
		return false;
	}

	reg = vgic_bytemap_get_reg(&vcpu->kvm->arch.vgic.irq_priority,
				   vcpu->vcpu_id, offset);
	vgic_reg_access(mmio, reg, offset,
		ACCESS_READ_VALUE | ACCESS_WRITE_VALUE);
	return false;
}

static bool handle_mmio_cfg_reg_dist(struct kvm_vcpu *vcpu,
				     struct kvm_exit_mmio *mmio,
				     phys_addr_t offset)
{
	u32 *reg;

	if (unlikely(offset < VGIC_NR_PRIVATE_IRQS / 4)) {
		vgic_reg_access(mmio, NULL, offset,
				ACCESS_READ_RAZ | ACCESS_WRITE_IGNORED);
		return false;
	}

	reg = vgic_bitmap_get_reg(&vcpu->kvm->arch.vgic.irq_cfg,
				  vcpu->vcpu_id, offset >> 1);

	return vgic_handle_cfg_reg(reg, mmio, offset);
}

/*
 * We use a compressed version of the MPIDR (all 32 bits in one 32-bit word)
 * when we store the target MPIDR written by the guest.
 */
static u32 compress_mpidr(unsigned long mpidr)
{
	u32 ret;

	ret = MPIDR_AFFINITY_LEVEL(mpidr, 0);
	ret |= MPIDR_AFFINITY_LEVEL(mpidr, 1) << 8;
	ret |= MPIDR_AFFINITY_LEVEL(mpidr, 2) << 16;
	ret |= MPIDR_AFFINITY_LEVEL(mpidr, 3) << 24;

	return ret;
}

static unsigned long uncompress_mpidr(u32 value)
{
	unsigned long mpidr;

	mpidr  = ((value >>  0) & 0xFF) << MPIDR_LEVEL_SHIFT(0);
	mpidr |= ((value >>  8) & 0xFF) << MPIDR_LEVEL_SHIFT(1);
	mpidr |= ((value >> 16) & 0xFF) << MPIDR_LEVEL_SHIFT(2);
	mpidr |= (u64)((value >> 24) & 0xFF) << MPIDR_LEVEL_SHIFT(3);

	return mpidr;
}

/*
 * Lookup the given MPIDR value to get the vcpu_id (if there is one)
 * and store that in the irq_spi_cpu[] array.
 * This limits the number of VCPUs to 255 for now, extending the data
 * type (or storing kvm_vcpu pointers) should lift the limit.
 * Store the original MPIDR value in an extra array to support read-as-written.
 * Unallocated MPIDRs are translated to a special value and caught
 * before any array accesses.
 */
static bool handle_mmio_route_reg(struct kvm_vcpu *vcpu,
				  struct kvm_exit_mmio *mmio,
				  phys_addr_t offset)
{
	struct kvm *kvm = vcpu->kvm;
	struct vgic_dist *dist = &kvm->arch.vgic;
	int spi;
	u32 reg;
	int vcpu_id;
	unsigned long *bmap, mpidr;

	/*
	 * The upper 32 bits of each 64 bit register are zero,
	 * as we don't support Aff3.
	 */
	if ((offset & 4)) {
		vgic_reg_access(mmio, NULL, offset,
				ACCESS_READ_RAZ | ACCESS_WRITE_IGNORED);
		return false;
	}

	/* This region only covers SPIs, so no handling of private IRQs here. */
	spi = offset / 8;

	/* get the stored MPIDR for this IRQ */
	mpidr = uncompress_mpidr(dist->irq_spi_mpidr[spi]);
	reg = mpidr;

	vgic_reg_access(mmio, &reg, offset,
			ACCESS_READ_VALUE | ACCESS_WRITE_VALUE);

	if (!mmio->is_write)
		return false;

	/*
	 * Now clear the currently assigned vCPU from the map, making room
	 * for the new one to be written below
	 */
	vcpu = kvm_mpidr_to_vcpu(kvm, mpidr);
	if (likely(vcpu)) {
		vcpu_id = vcpu->vcpu_id;
		bmap = vgic_bitmap_get_shared_map(&dist->irq_spi_target[vcpu_id]);
		__clear_bit(spi, bmap);
	}

	dist->irq_spi_mpidr[spi] = compress_mpidr(reg);
	vcpu = kvm_mpidr_to_vcpu(kvm, reg & MPIDR_HWID_BITMASK);

	/*
	 * The spec says that non-existent MPIDR values should not be
	 * forwarded to any existent (v)CPU, but should be able to become
	 * pending anyway. We simply keep the irq_spi_target[] array empty, so
	 * the interrupt will never be injected.
	 * irq_spi_cpu[irq] gets a magic value in this case.
	 */
	if (likely(vcpu)) {
		vcpu_id = vcpu->vcpu_id;
		dist->irq_spi_cpu[spi] = vcpu_id;
		bmap = vgic_bitmap_get_shared_map(&dist->irq_spi_target[vcpu_id]);
		__set_bit(spi, bmap);
	} else {
		dist->irq_spi_cpu[spi] = VCPU_NOT_ALLOCATED;
	}

	vgic_update_state(kvm);

	return true;
}

/*
 * We should be careful about promising too much when a guest reads
 * this register. Don't claim to be like any hardware implementation,
 * but just report the GIC as version 3 - which is what a Linux guest
 * would check.
 */
static bool handle_mmio_idregs(struct kvm_vcpu *vcpu,
			       struct kvm_exit_mmio *mmio,
			       phys_addr_t offset)
{
	u32 reg = 0;

	switch (offset + GICD_IDREGS) {
	case GICD_PIDR2:
		reg = 0x3b;
		break;
	}

	vgic_reg_access(mmio, &reg, offset,
			ACCESS_READ_VALUE | ACCESS_WRITE_IGNORED);

	return false;
}

static const struct vgic_io_range vgic_v3_dist_ranges[] = {
	{
		.base           = GICD_CTLR,
		.len            = 0x04,
		.bits_per_irq   = 0,
		.handle_mmio    = handle_mmio_ctlr,
	},
	{
		.base           = GICD_TYPER,
		.len            = 0x04,
		.bits_per_irq   = 0,
		.handle_mmio    = handle_mmio_typer,
	},
	{
		.base           = GICD_IIDR,
		.len            = 0x04,
		.bits_per_irq   = 0,
		.handle_mmio    = handle_mmio_iidr,
	},
	{
		/* this register is optional, it is RAZ/WI if not implemented */
		.base           = GICD_STATUSR,
		.len            = 0x04,
		.bits_per_irq   = 0,
		.handle_mmio    = handle_mmio_raz_wi,
	},
	{
		/* this write only register is WI when TYPER.MBIS=0 */
		.base		= GICD_SETSPI_NSR,
		.len		= 0x04,
		.bits_per_irq	= 0,
		.handle_mmio	= handle_mmio_raz_wi,
	},
	{
		/* this write only register is WI when TYPER.MBIS=0 */
		.base		= GICD_CLRSPI_NSR,
		.len		= 0x04,
		.bits_per_irq	= 0,
		.handle_mmio	= handle_mmio_raz_wi,
	},
	{
		/* this is RAZ/WI when DS=1 */
		.base		= GICD_SETSPI_SR,
		.len		= 0x04,
		.bits_per_irq	= 0,
		.handle_mmio	= handle_mmio_raz_wi,
	},
	{
		/* this is RAZ/WI when DS=1 */
		.base		= GICD_CLRSPI_SR,
		.len		= 0x04,
		.bits_per_irq	= 0,
		.handle_mmio	= handle_mmio_raz_wi,
	},
	{
		.base		= GICD_IGROUPR,
		.len		= 0x80,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_rao_wi,
	},
	{
		.base		= GICD_ISENABLER,
		.len		= 0x80,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_set_enable_reg_dist,
	},
	{
		.base		= GICD_ICENABLER,
		.len		= 0x80,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_clear_enable_reg_dist,
	},
	{
		.base		= GICD_ISPENDR,
		.len		= 0x80,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_set_pending_reg_dist,
	},
	{
		.base		= GICD_ICPENDR,
		.len		= 0x80,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_clear_pending_reg_dist,
	},
	{
		.base		= GICD_ISACTIVER,
		.len		= 0x80,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_set_active_reg_dist,
	},
	{
		.base		= GICD_ICACTIVER,
		.len		= 0x80,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_clear_active_reg_dist,
	},
	{
		.base		= GICD_IPRIORITYR,
		.len		= 0x400,
		.bits_per_irq	= 8,
		.handle_mmio	= handle_mmio_priority_reg_dist,
	},
	{
		/* TARGETSRn is RES0 when ARE=1 */
		.base		= GICD_ITARGETSR,
		.len		= 0x400,
		.bits_per_irq	= 8,
		.handle_mmio	= handle_mmio_raz_wi,
	},
	{
		.base		= GICD_ICFGR,
		.len		= 0x100,
		.bits_per_irq	= 2,
		.handle_mmio	= handle_mmio_cfg_reg_dist,
	},
	{
		/* this is RAZ/WI when DS=1 */
		.base		= GICD_IGRPMODR,
		.len		= 0x80,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_raz_wi,
	},
	{
		/* this is RAZ/WI when DS=1 */
		.base		= GICD_NSACR,
		.len		= 0x100,
		.bits_per_irq	= 2,
		.handle_mmio	= handle_mmio_raz_wi,
	},
	{
		/* this is RAZ/WI when ARE=1 */
		.base		= GICD_SGIR,
		.len		= 0x04,
		.handle_mmio	= handle_mmio_raz_wi,
	},
	{
		/* this is RAZ/WI when ARE=1 */
		.base		= GICD_CPENDSGIR,
		.len		= 0x10,
		.handle_mmio	= handle_mmio_raz_wi,
	},
	{
		/* this is RAZ/WI when ARE=1 */
		.base           = GICD_SPENDSGIR,
		.len            = 0x10,
		.handle_mmio    = handle_mmio_raz_wi,
	},
	{
		.base		= GICD_IROUTER + 0x100,
		.len		= 0x1ee0,
		.bits_per_irq	= 64,
		.handle_mmio	= handle_mmio_route_reg,
	},
	{
		.base           = GICD_IDREGS,
		.len            = 0x30,
		.bits_per_irq   = 0,
		.handle_mmio    = handle_mmio_idregs,
	},
	{},
};

static bool handle_mmio_ctlr_redist(struct kvm_vcpu *vcpu,
				    struct kvm_exit_mmio *mmio,
				    phys_addr_t offset)
{
	/* since we don't support LPIs, this register is zero for now */
	vgic_reg_access(mmio, NULL, offset,
			ACCESS_READ_RAZ | ACCESS_WRITE_IGNORED);
	return false;
}

static bool handle_mmio_typer_redist(struct kvm_vcpu *vcpu,
				     struct kvm_exit_mmio *mmio,
				     phys_addr_t offset)
{
	u32 reg;
	u64 mpidr;
	struct kvm_vcpu *redist_vcpu = mmio->private;
	int target_vcpu_id = redist_vcpu->vcpu_id;

	/* the upper 32 bits contain the affinity value */
	if ((offset & ~3) == 4) {
		mpidr = kvm_vcpu_get_mpidr_aff(redist_vcpu);
		reg = compress_mpidr(mpidr);

		vgic_reg_access(mmio, &reg, offset,
				ACCESS_READ_VALUE | ACCESS_WRITE_IGNORED);
		return false;
	}

	reg = redist_vcpu->vcpu_id << 8;
	if (target_vcpu_id == atomic_read(&vcpu->kvm->online_vcpus) - 1)
		reg |= GICR_TYPER_LAST;
	vgic_reg_access(mmio, &reg, offset,
			ACCESS_READ_VALUE | ACCESS_WRITE_IGNORED);
	return false;
}

static bool handle_mmio_set_enable_reg_redist(struct kvm_vcpu *vcpu,
					      struct kvm_exit_mmio *mmio,
					      phys_addr_t offset)
{
	struct kvm_vcpu *redist_vcpu = mmio->private;

	return vgic_handle_enable_reg(vcpu->kvm, mmio, offset,
				      redist_vcpu->vcpu_id,
				      ACCESS_WRITE_SETBIT);
}

static bool handle_mmio_clear_enable_reg_redist(struct kvm_vcpu *vcpu,
						struct kvm_exit_mmio *mmio,
						phys_addr_t offset)
{
	struct kvm_vcpu *redist_vcpu = mmio->private;

	return vgic_handle_enable_reg(vcpu->kvm, mmio, offset,
				      redist_vcpu->vcpu_id,
				      ACCESS_WRITE_CLEARBIT);
}

static bool handle_mmio_set_active_reg_redist(struct kvm_vcpu *vcpu,
					      struct kvm_exit_mmio *mmio,
					      phys_addr_t offset)
{
	struct kvm_vcpu *redist_vcpu = mmio->private;

	return vgic_handle_set_active_reg(vcpu->kvm, mmio, offset,
					  redist_vcpu->vcpu_id);
}

static bool handle_mmio_clear_active_reg_redist(struct kvm_vcpu *vcpu,
						struct kvm_exit_mmio *mmio,
						phys_addr_t offset)
{
	struct kvm_vcpu *redist_vcpu = mmio->private;

	return vgic_handle_clear_active_reg(vcpu->kvm, mmio, offset,
					     redist_vcpu->vcpu_id);
}

static bool handle_mmio_set_pending_reg_redist(struct kvm_vcpu *vcpu,
					       struct kvm_exit_mmio *mmio,
					       phys_addr_t offset)
{
	struct kvm_vcpu *redist_vcpu = mmio->private;

	return vgic_handle_set_pending_reg(vcpu->kvm, mmio, offset,
					   redist_vcpu->vcpu_id);
}

static bool handle_mmio_clear_pending_reg_redist(struct kvm_vcpu *vcpu,
						 struct kvm_exit_mmio *mmio,
						 phys_addr_t offset)
{
	struct kvm_vcpu *redist_vcpu = mmio->private;

	return vgic_handle_clear_pending_reg(vcpu->kvm, mmio, offset,
					     redist_vcpu->vcpu_id);
}

static bool handle_mmio_priority_reg_redist(struct kvm_vcpu *vcpu,
					    struct kvm_exit_mmio *mmio,
					    phys_addr_t offset)
{
	struct kvm_vcpu *redist_vcpu = mmio->private;
	u32 *reg;

	reg = vgic_bytemap_get_reg(&vcpu->kvm->arch.vgic.irq_priority,
				   redist_vcpu->vcpu_id, offset);
	vgic_reg_access(mmio, reg, offset,
			ACCESS_READ_VALUE | ACCESS_WRITE_VALUE);
	return false;
}

static bool handle_mmio_cfg_reg_redist(struct kvm_vcpu *vcpu,
				       struct kvm_exit_mmio *mmio,
				       phys_addr_t offset)
{
	struct kvm_vcpu *redist_vcpu = mmio->private;

	u32 *reg = vgic_bitmap_get_reg(&vcpu->kvm->arch.vgic.irq_cfg,
				       redist_vcpu->vcpu_id, offset >> 1);

	return vgic_handle_cfg_reg(reg, mmio, offset);
}

#define SGI_base(x) ((x) + SZ_64K)

static const struct vgic_io_range vgic_redist_ranges[] = {
	{
		.base           = GICR_CTLR,
		.len            = 0x04,
		.bits_per_irq   = 0,
		.handle_mmio    = handle_mmio_ctlr_redist,
	},
	{
		.base           = GICR_TYPER,
		.len            = 0x08,
		.bits_per_irq   = 0,
		.handle_mmio    = handle_mmio_typer_redist,
	},
	{
		.base           = GICR_IIDR,
		.len            = 0x04,
		.bits_per_irq   = 0,
		.handle_mmio    = handle_mmio_iidr,
	},
	{
		.base           = GICR_WAKER,
		.len            = 0x04,
		.bits_per_irq   = 0,
		.handle_mmio    = handle_mmio_raz_wi,
	},
	{
		.base           = GICR_IDREGS,
		.len            = 0x30,
		.bits_per_irq   = 0,
		.handle_mmio    = handle_mmio_idregs,
	},
	{
		.base		= SGI_base(GICR_IGROUPR0),
		.len		= 0x04,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_rao_wi,
	},
	{
		.base		= SGI_base(GICR_ISENABLER0),
		.len		= 0x04,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_set_enable_reg_redist,
	},
	{
		.base		= SGI_base(GICR_ICENABLER0),
		.len		= 0x04,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_clear_enable_reg_redist,
	},
	{
		.base		= SGI_base(GICR_ISPENDR0),
		.len		= 0x04,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_set_pending_reg_redist,
	},
	{
		.base		= SGI_base(GICR_ICPENDR0),
		.len		= 0x04,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_clear_pending_reg_redist,
	},
	{
		.base		= SGI_base(GICR_ISACTIVER0),
		.len		= 0x04,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_set_active_reg_redist,
	},
	{
		.base		= SGI_base(GICR_ICACTIVER0),
		.len		= 0x04,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_clear_active_reg_redist,
	},
	{
		.base		= SGI_base(GICR_IPRIORITYR0),
		.len		= 0x20,
		.bits_per_irq	= 8,
		.handle_mmio	= handle_mmio_priority_reg_redist,
	},
	{
		.base		= SGI_base(GICR_ICFGR0),
		.len		= 0x08,
		.bits_per_irq	= 2,
		.handle_mmio	= handle_mmio_cfg_reg_redist,
	},
	{
		.base		= SGI_base(GICR_IGRPMODR0),
		.len		= 0x04,
		.bits_per_irq	= 1,
		.handle_mmio	= handle_mmio_raz_wi,
	},
	{
		.base		= SGI_base(GICR_NSACR),
		.len		= 0x04,
		.handle_mmio	= handle_mmio_raz_wi,
	},
	{},
};

static bool vgic_v3_queue_sgi(struct kvm_vcpu *vcpu, int irq)
{
	if (vgic_queue_irq(vcpu, 0, irq)) {
		vgic_dist_irq_clear_pending(vcpu, irq);
		vgic_cpu_irq_clear(vcpu, irq);
		return true;
	}

	return false;
}

static int vgic_v3_map_resources(struct kvm *kvm,
				 const struct vgic_params *params)
{
	int ret = 0;
	struct vgic_dist *dist = &kvm->arch.vgic;
	gpa_t rdbase = dist->vgic_redist_base;
	struct vgic_io_device *iodevs = NULL;
	int i;

	if (!irqchip_in_kernel(kvm))
		return 0;

	mutex_lock(&kvm->lock);

	if (vgic_ready(kvm))
		goto out;

	if (IS_VGIC_ADDR_UNDEF(dist->vgic_dist_base) ||
	    IS_VGIC_ADDR_UNDEF(dist->vgic_redist_base)) {
		kvm_err("Need to set vgic distributor addresses first\n");
		ret = -ENXIO;
		goto out;
	}

	/*
	 * For a VGICv3 we require the userland to explicitly initialize
	 * the VGIC before we need to use it.
	 */
	if (!vgic_initialized(kvm)) {
		ret = -EBUSY;
		goto out;
	}

	ret = vgic_register_kvm_io_dev(kvm, dist->vgic_dist_base,
				       GIC_V3_DIST_SIZE, vgic_v3_dist_ranges,
				       -1, &dist->dist_iodev);
	if (ret)
		goto out;

	iodevs = kcalloc(dist->nr_cpus, sizeof(iodevs[0]), GFP_KERNEL);
	if (!iodevs) {
		ret = -ENOMEM;
		goto out_unregister;
	}

	for (i = 0; i < dist->nr_cpus; i++) {
		ret = vgic_register_kvm_io_dev(kvm, rdbase,
					       SZ_128K, vgic_redist_ranges,
					       i, &iodevs[i]);
		if (ret)
			goto out_unregister;
		rdbase += GIC_V3_REDIST_SIZE;
	}

	dist->redist_iodevs = iodevs;
	dist->ready = true;
	goto out;

out_unregister:
	kvm_io_bus_unregister_dev(kvm, KVM_MMIO_BUS, &dist->dist_iodev.dev);
	if (iodevs) {
		for (i = 0; i < dist->nr_cpus; i++) {
			if (iodevs[i].dev.ops)
				kvm_io_bus_unregister_dev(kvm, KVM_MMIO_BUS,
							  &iodevs[i].dev);
		}
	}

out:
	if (ret)
		kvm_vgic_destroy(kvm);
	mutex_unlock(&kvm->lock);
	return ret;
}

static int vgic_v3_init_model(struct kvm *kvm)
{
	int i;
	u32 mpidr;
	struct vgic_dist *dist = &kvm->arch.vgic;
	int nr_spis = dist->nr_irqs - VGIC_NR_PRIVATE_IRQS;

	dist->irq_spi_mpidr = kcalloc(nr_spis, sizeof(dist->irq_spi_mpidr[0]),
				      GFP_KERNEL);

	if (!dist->irq_spi_mpidr)
		return -ENOMEM;

	/* Initialize the target VCPUs for each IRQ to VCPU 0 */
	mpidr = compress_mpidr(kvm_vcpu_get_mpidr_aff(kvm_get_vcpu(kvm, 0)));
	for (i = VGIC_NR_PRIVATE_IRQS; i < dist->nr_irqs; i++) {
		dist->irq_spi_cpu[i - VGIC_NR_PRIVATE_IRQS] = 0;
		dist->irq_spi_mpidr[i - VGIC_NR_PRIVATE_IRQS] = mpidr;
		vgic_bitmap_set_irq_val(dist->irq_spi_target, 0, i, 1);
	}

	return 0;
}

/* GICv3 does not keep track of SGI sources anymore. */
static void vgic_v3_add_sgi_source(struct kvm_vcpu *vcpu, int irq, int source)
{
}

void vgic_v3_init_emulation(struct kvm *kvm)
{
	struct vgic_dist *dist = &kvm->arch.vgic;

	dist->vm_ops.queue_sgi = vgic_v3_queue_sgi;
	dist->vm_ops.add_sgi_source = vgic_v3_add_sgi_source;
	dist->vm_ops.init_model = vgic_v3_init_model;
	dist->vm_ops.map_resources = vgic_v3_map_resources;

	kvm->arch.max_vcpus = KVM_MAX_VCPUS;
}

/*
 * Compare a given affinity (level 1-3 and a level 0 mask, from the SGI
 * generation register ICC_SGI1R_EL1) with a given VCPU.
 * If the VCPU's MPIDR matches, return the level0 affinity, otherwise
 * return -1.
 */
static int match_mpidr(u64 sgi_aff, u16 sgi_cpu_mask, struct kvm_vcpu *vcpu)
{
	unsigned long affinity;
	int level0;

	/*
	 * Split the current VCPU's MPIDR into affinity level 0 and the
	 * rest as this is what we have to compare against.
	 */
	affinity = kvm_vcpu_get_mpidr_aff(vcpu);
	level0 = MPIDR_AFFINITY_LEVEL(affinity, 0);
	affinity &= ~MPIDR_LEVEL_MASK;

	/* bail out if the upper three levels don't match */
	if (sgi_aff != affinity)
		return -1;

	/* Is this VCPU's bit set in the mask ? */
	if (!(sgi_cpu_mask & BIT(level0)))
		return -1;

	return level0;
}

#define SGI_AFFINITY_LEVEL(reg, level) \
	((((reg) & ICC_SGI1R_AFFINITY_## level ##_MASK) \
	>> ICC_SGI1R_AFFINITY_## level ##_SHIFT) << MPIDR_LEVEL_SHIFT(level))

/**
 * vgic_v3_dispatch_sgi - handle SGI requests from VCPUs
 * @vcpu: The VCPU requesting a SGI
 * @reg: The value written into the ICC_SGI1R_EL1 register by that VCPU
 *
 * With GICv3 (and ARE=1) CPUs trigger SGIs by writing to a system register.
 * This will trap in sys_regs.c and call this function.
 * This ICC_SGI1R_EL1 register contains the upper three affinity levels of the
 * target processors as well as a bitmask of 16 Aff0 CPUs.
 * If the interrupt routing mode bit is not set, we iterate over all VCPUs to
 * check for matching ones. If this bit is set, we signal all, but not the
 * calling VCPU.
 */
void vgic_v3_dispatch_sgi(struct kvm_vcpu *vcpu, u64 reg)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_vcpu *c_vcpu;
	struct vgic_dist *dist = &kvm->arch.vgic;
	u16 target_cpus;
	u64 mpidr;
	int sgi, c;
	int vcpu_id = vcpu->vcpu_id;
	bool broadcast;
	int updated = 0;

	sgi = (reg & ICC_SGI1R_SGI_ID_MASK) >> ICC_SGI1R_SGI_ID_SHIFT;
	broadcast = reg & BIT(ICC_SGI1R_IRQ_ROUTING_MODE_BIT);
	target_cpus = (reg & ICC_SGI1R_TARGET_LIST_MASK) >> ICC_SGI1R_TARGET_LIST_SHIFT;
	mpidr = SGI_AFFINITY_LEVEL(reg, 3);
	mpidr |= SGI_AFFINITY_LEVEL(reg, 2);
	mpidr |= SGI_AFFINITY_LEVEL(reg, 1);

	/*
	 * We take the dist lock here, because we come from the sysregs
	 * code path and not from the MMIO one (which already takes the lock).
	 */
	spin_lock(&dist->lock);

	/*
	 * We iterate over all VCPUs to find the MPIDRs matching the request.
	 * If we have handled one CPU, we clear it's bit to detect early
	 * if we are already finished. This avoids iterating through all
	 * VCPUs when most of the times we just signal a single VCPU.
	 */
	kvm_for_each_vcpu(c, c_vcpu, kvm) {

		/* Exit early if we have dealt with all requested CPUs */
		if (!broadcast && target_cpus == 0)
			break;

		 /* Don't signal the calling VCPU */
		if (broadcast && c == vcpu_id)
			continue;

		if (!broadcast) {
			int level0;

			level0 = match_mpidr(mpidr, target_cpus, c_vcpu);
			if (level0 == -1)
				continue;

			/* remove this matching VCPU from the mask */
			target_cpus &= ~BIT(level0);
		}

		/* Flag the SGI as pending */
		vgic_dist_irq_set_pending(c_vcpu, sgi);
		updated = 1;
		kvm_debug("SGI%d from CPU%d to CPU%d\n", sgi, vcpu_id, c);
	}
	if (updated)
		vgic_update_state(vcpu->kvm);
	spin_unlock(&dist->lock);
	if (updated)
		vgic_kick_vcpus(vcpu->kvm);
}

static bool access_gic_ctlr(struct kvm_vcpu *vcpu,
			    const struct sys_reg_params *p,
			    const struct sys_reg_desc *r)
{
	u64 val;
	struct vgic_v3_cpu_if *vgicv3 = &vcpu->arch.vgic_cpu.vgic_v3;

	if (p->is_write) {
		val = *p->val;

		vgicv3->vgic_vmcr &= ~(ICH_VMCR_CBPR|ICH_VMCR_EOIM);
		vgicv3->vgic_vmcr |= (val << (ICH_VMCR_CBPR_SHIFT -
						ICC_CTLR_EL1_CBPR_SHIFT)) &
					ICH_VMCR_CBPR;
		vgicv3->vgic_vmcr |= (val << (ICH_VMCR_EOIM_SHIFT -
						ICC_CTLR_EL1_EOImode_SHIFT)) &
					ICH_VMCR_EOIM;
	} else {
		asm volatile("mrs_s %0," __stringify(ICC_IAR1_EL1)
			     : "=r" (val));
		val &= (ICC_CTLR_EL1_A3V | ICC_CTLR_EL1_SEIS |
			ICC_CTLR_EL1_IDbits_MASK | ICC_CTLR_EL1_PRIbits_MASK);
		val |= (vgicv3->vgic_vmcr & ICH_VMCR_CBPR) >>
			(ICH_VMCR_CBPR_SHIFT - ICC_CTLR_EL1_CBPR_SHIFT);
		val |= (vgicv3->vgic_vmcr & ICH_VMCR_EOIM) >>
			(ICH_VMCR_EOIM_SHIFT - ICC_CTLR_EL1_EOImode_SHIFT);

		*p->val = val;
	}

	return true;
}

static bool access_gic_pmr(struct kvm_vcpu *vcpu,
			   const struct sys_reg_params *p,
			   const struct sys_reg_desc *r)
{
	struct vgic_v3_cpu_if *vgicv3 = &vcpu->arch.vgic_cpu.vgic_v3;

	if (p->is_write) {
		vgicv3->vgic_vmcr &= ~ICH_VMCR_PMR_MASK;
		vgicv3->vgic_vmcr |= (*p->val << ICH_VMCR_PMR_SHIFT) &
					ICH_VMCR_PMR_MASK;
	} else {
		*p->val = (vgicv3->vgic_vmcr & ICH_VMCR_PMR_MASK) >>
			  ICH_VMCR_PMR_SHIFT;
	}

	return true;
}

static bool access_gic_bpr0(struct kvm_vcpu *vcpu,
			    const struct sys_reg_params *p,
			    const struct sys_reg_desc *r)
{
	struct vgic_v3_cpu_if *vgicv3 = &vcpu->arch.vgic_cpu.vgic_v3;

	if (p->is_write) {
		vgicv3->vgic_vmcr &= ~ICH_VMCR_BPR0_MASK;
		vgicv3->vgic_vmcr |= (*p->val << ICH_VMCR_BPR0_SHIFT) &
				     ICH_VMCR_BPR0_MASK;
	} else {
		*p->val = (vgicv3->vgic_vmcr & ICH_VMCR_BPR0_MASK) >>
			  ICH_VMCR_BPR0_SHIFT;
	}

	return true;
}

static bool access_gic_bpr1(struct kvm_vcpu *vcpu,
			    const struct sys_reg_params *p,
			    const struct sys_reg_desc *r)
{
	struct vgic_v3_cpu_if *vgicv3 = &vcpu->arch.vgic_cpu.vgic_v3;

	if (p->is_write) {
		vgicv3->vgic_vmcr &= ~ICH_VMCR_BPR1_MASK;
		vgicv3->vgic_vmcr |= (*p->val << ICH_VMCR_BPR1_SHIFT) &
				     ICH_VMCR_BPR1_MASK;
	} else {
		*p->val = (vgicv3->vgic_vmcr & ICH_VMCR_BPR1_MASK) >>
			  ICH_VMCR_BPR1_SHIFT;
	}

	return true;
}

static bool access_gic_grpen0(struct kvm_vcpu *vcpu,
			      const struct sys_reg_params *p,
			      const struct sys_reg_desc *r)
{
	struct vgic_v3_cpu_if *vgicv3 = &vcpu->arch.vgic_cpu.vgic_v3;

	if (p->is_write) {
		vgicv3->vgic_vmcr &= ~ICH_VMCR_ENG0;
		vgicv3->vgic_vmcr |= (*p->val << ICH_VMCR_ENG0_SHIFT) &
				     ICH_VMCR_ENG0;
	} else {
		*p->val = (vgicv3->vgic_vmcr & ICH_VMCR_ENG0) >>
			  ICH_VMCR_ENG0_SHIFT;
	}

	return true;
}

static bool access_gic_grpen1(struct kvm_vcpu *vcpu,
			      const struct sys_reg_params *p,
			      const struct sys_reg_desc *r)
{
	struct vgic_v3_cpu_if *vgicv3 = &vcpu->arch.vgic_cpu.vgic_v3;

	if (p->is_write) {
		vgicv3->vgic_vmcr &= ~ICH_VMCR_ENG1;
		vgicv3->vgic_vmcr |= (*p->val << ICH_VMCR_ENG1_SHIFT) &
				     ICH_VMCR_ENG1;
	} else {
		*p->val = (vgicv3->vgic_vmcr & ICH_VMCR_ENG1) >>
			  ICH_VMCR_ENG1_SHIFT;
	}

	return true;
}

static bool access_gic_ap0r(struct kvm_vcpu *vcpu,
			      const struct sys_reg_params *p,
			      const struct sys_reg_desc *r)
{
	struct vgic_v3_cpu_if *vgicv3 = &vcpu->arch.vgic_cpu.vgic_v3;
	u8 idx = r->Op2 & 3;

	if (p->is_write)
		vgicv3->vgic_ap0r[idx] = *p->val;
	else
		*p->val = vgicv3->vgic_ap0r[idx];

	return true;
}

static bool access_gic_ap1r(struct kvm_vcpu *vcpu,
			      const struct sys_reg_params *p,
			      const struct sys_reg_desc *r)
{
	struct vgic_v3_cpu_if *vgicv3 = &vcpu->arch.vgic_cpu.vgic_v3;
	u8 idx = r->Op2 & 3;

	if (p->is_write)
		vgicv3->vgic_ap1r[idx] = *p->val;
	else
		*p->val = vgicv3->vgic_ap1r[idx];

	return true;
}

static const struct sys_reg_desc gic_v3_icc_reg_descs[] = {
	/* ICC_PMR_EL1 */
	{ Op0(0b11), Op1(0b000), CRn(0b0100), CRm(0b0110), Op2(0b000),
	  access_gic_pmr },
	/* ICC_BPR0_EL1 */
	{ Op0(0b11), Op1(0b000), CRn(0b1100), CRm(0b1000), Op2(0b011),
	  access_gic_bpr0 },
	/* ICC_AP0R0_EL1 */
	{ Op0(0b11), Op1(0b000), CRn(0b1100), CRm(0b1000), Op2(0b100),
	  access_gic_ap0r },
	/* ICC_AP0R1_EL1 */
	{ Op0(0b11), Op1(0b000), CRn(0b1100), CRm(0b1000), Op2(0b101),
	  access_gic_ap0r },
	/* ICC_AP0R2_EL1 */
	{ Op0(0b11), Op1(0b000), CRn(0b1100), CRm(0b1000), Op2(0b110),
	  access_gic_ap0r },
	/* ICC_AP0R3_EL1 */
	{ Op0(0b11), Op1(0b000), CRn(0b1100), CRm(0b1000), Op2(0b111),
	  access_gic_ap0r },
	/* ICC_AP1R0_EL1 */
	{ Op0(0b11), Op1(0b000), CRn(0b1100), CRm(0b1001), Op2(0b000),
	  access_gic_ap1r },
	/* ICC_AP1R1_EL1 */
	{ Op0(0b11), Op1(0b000), CRn(0b1100), CRm(0b1001), Op2(0b001),
	  access_gic_ap1r },
	/* ICC_AP1R2_EL1 */
	{ Op0(0b11), Op1(0b000), CRn(0b1100), CRm(0b1001), Op2(0b010),
	  access_gic_ap1r },
	/* ICC_AP1R3_EL1 */
	{ Op0(0b11), Op1(0b000), CRn(0b1100), CRm(0b1001), Op2(0b011),
	  access_gic_ap1r },
	/* ICC_BPR1_EL1 */
	{ Op0(0b11), Op1(0b000), CRn(0b1100), CRm(0b1100), Op2(0b011),
	  access_gic_bpr1 },
	/* ICC_CTLR_EL1 */
	{ Op0(0b11), Op1(0b000), CRn(0b1100), CRm(0b1100), Op2(0b100),
	  access_gic_ctlr },
	/* ICC_IGRPEN0_EL1 */
	{ Op0(0b11), Op1(0b000), CRn(0b1100), CRm(0b1100), Op2(0b110),
	  access_gic_grpen0 },
	/* ICC_GRPEN1_EL1 */
	{ Op0(0b11), Op1(0b000), CRn(0b1100), CRm(0b1100), Op2(0b111),
	  access_gic_grpen1 },
};

static int vgic_v3_cpu_regs_access(struct kvm_vcpu *vcpu, u64 id, u64 *reg,
				   bool is_write)
{
	struct sys_reg_params params;
	const struct sys_reg_desc *r;

	params.val = (u_long *)reg;
	params.is_write = is_write;
	params.is_aarch32 = false;
	params.is_32bit = false;

	r = find_reg_by_id(id, &params, gic_v3_icc_reg_descs,
			   ARRAY_SIZE(gic_v3_icc_reg_descs));
	if (!r)
		return -ENXIO;

	/* Ensure that VCPU is not running */
	if (unlikely(vcpu->cpu != -1))
		return -EBUSY;

	return r->access(vcpu, &params, r) ? 0 : -EINVAL;
}

static u32 vgic_v3_get_reg_size(u32 group, u32 offset)
{
	switch (group) {
	case KVM_DEV_ARM_VGIC_GRP_DIST_REGS:
		if (offset >= GICD_IROUTER && offset <= GICD_IROUTER1019)
			return 8;
		else
			return 4;
		break;

	case KVM_DEV_ARM_VGIC_GRP_REDIST_REGS:
		if ((offset == GICR_TYPER) ||
		    (offset >= GICR_SETLPIR && offset <= GICR_INVALLR))
			return 8;
		else
			return 4;
		break;

	default:
		BUG();
	}
}

static int vgic_v3_attr_regs_access(struct kvm_device *dev,
				    struct kvm_device_attr *attr,
				    u64 *reg, bool is_write)
{
	const struct vgic_io_range *ranges;
	phys_addr_t offset;
	struct kvm_vcpu *vcpu;
	u64 cpuid, regid;
	struct vgic_dist *vgic = &dev->kvm->arch.vgic;
	struct kvm_exit_mmio mmio;
	__le64 data;
	int ret;

	offset = attr->attr & KVM_DEV_ARM_VGIC_OFFSET_MASK;
	cpuid = attr->attr >> KVM_DEV_ARM_VGIC_CPUID_SHIFT;

	/* Convert affinity ID from our packed to normal form */
	cpuid = (cpuid & 0x00ffffff) | ((cpuid & 0xff000000) << 8);
	vcpu = kvm_mpidr_to_vcpu(dev->kvm, cpuid);
	if (!vcpu)
		return -EINVAL;

	switch (attr->group) {
	case KVM_DEV_ARM_VGIC_GRP_DIST_REGS:
		mmio.phys_addr = vgic->vgic_dist_base + offset;
		ranges = vgic_v3_dist_ranges;
		break;
	case KVM_DEV_ARM_VGIC_GRP_REDIST_REGS:
		mmio.phys_addr = vgic->vgic_redist_base + offset;
		ranges = vgic_redist_ranges;
		break;
	case KVM_DEV_ARM_VGIC_GRP_CPU_SYSREGS:
		/*
		 * Our register ID is missing size specifier, expected by
		 * index_to_params()
		 */
		regid = (attr->attr & KVM_DEV_ARM_VGIC_SYSREG_MASK) |
			KVM_REG_SIZE_U64;
		return vgic_v3_cpu_regs_access(vcpu, regid, reg, is_write);
	default:
		return -ENXIO;
	}

	data = cpu_to_le64(*reg);

	mmio.len = vgic_v3_get_reg_size(attr->group, offset);
	mmio.is_write = is_write;
	mmio.data = &data;
	mmio.private = vcpu; /* Redistributor handlers expect this */

	ret = vgic_attr_regs_access(vcpu, ranges, &mmio, offset);

	*reg = le64_to_cpu(data);
	return ret;
}

static int vgic_v3_create(struct kvm_device *dev, u32 type)
{
	return kvm_vgic_create(dev->kvm, type);
}

static void vgic_v3_destroy(struct kvm_device *dev)
{
	kfree(dev);
}

static int vgic_v3_set_attr(struct kvm_device *dev,
			    struct kvm_device_attr *attr)
{
	u64 __user *uaddr = (u64 __user *)(long)attr->addr;
	u64 reg;
	int ret;

	ret = vgic_set_common_attr(dev, attr);
	if (ret != -ENXIO)
		return ret;

	if (get_user(reg, uaddr))
		return -EFAULT;

	return vgic_v3_attr_regs_access(dev, attr, &reg, true);
}

static int vgic_v3_get_attr(struct kvm_device *dev,
			    struct kvm_device_attr *attr)
{
	u64 __user *uaddr = (u64 __user *)(long)attr->addr;
	u64 reg = 0;
	int ret;

	ret = vgic_get_common_attr(dev, attr);
	if (ret != -ENXIO)
		return ret;

	ret = vgic_v3_attr_regs_access(dev, attr, &reg, false);
	if (ret)
		return ret;

	return put_user(reg, uaddr);
}

static int vgic_v3_has_attr(struct kvm_device *dev,
			    struct kvm_device_attr *attr)
{
	phys_addr_t offset;
	struct sys_reg_params params;
	u64 regid;

	switch (attr->group) {
	case KVM_DEV_ARM_VGIC_GRP_ADDR:
		switch (attr->attr) {
		case KVM_VGIC_V2_ADDR_TYPE_DIST:
		case KVM_VGIC_V2_ADDR_TYPE_CPU:
			return -ENXIO;
		case KVM_VGIC_V3_ADDR_TYPE_DIST:
		case KVM_VGIC_V3_ADDR_TYPE_REDIST:
			return 0;
		}
		break;
	case KVM_DEV_ARM_VGIC_GRP_DIST_REGS:
		offset = attr->attr & KVM_DEV_ARM_VGIC_OFFSET_MASK;
		return vgic_has_attr_regs(vgic_v3_dist_ranges, offset);
	case KVM_DEV_ARM_VGIC_GRP_REDIST_REGS:
		offset = attr->attr & KVM_DEV_ARM_VGIC_OFFSET_MASK;
		return vgic_has_attr_regs(vgic_redist_ranges, offset);
	case KVM_DEV_ARM_VGIC_GRP_CPU_SYSREGS:
		regid = (attr->attr & KVM_DEV_ARM_VGIC_SYSREG_MASK) |
			KVM_REG_SIZE_U64;
		return find_reg_by_id(regid, &params, gic_v3_icc_reg_descs,
				      ARRAY_SIZE(gic_v3_icc_reg_descs)) ?
			0 : -ENXIO;
	case KVM_DEV_ARM_VGIC_GRP_NR_IRQS:
		return 0;
	case KVM_DEV_ARM_VGIC_GRP_CTRL:
		switch (attr->attr) {
		case KVM_DEV_ARM_VGIC_CTRL_INIT:
			return 0;
		}
	}
	return -ENXIO;
}

struct kvm_device_ops kvm_arm_vgic_v3_ops = {
	.name = "kvm-arm-vgic-v3",
	.create = vgic_v3_create,
	.destroy = vgic_v3_destroy,
	.set_attr = vgic_v3_set_attr,
	.get_attr = vgic_v3_get_attr,
	.has_attr = vgic_v3_has_attr,
};
