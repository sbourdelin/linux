// SPDX-License-Identifier: GPL-2.0
/*
 * USBSSP device controller driver
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak
 * Some code borrowed from the Linux XHCI driver.
 */


#include "gadget.h"

#define usb_endpoint_out(ep_dir)	(!((ep_dir) & USB_DIR_IN))
#define USBSSP_INIT_VALUE 0x0

/* Add verbose debugging later, just print everything for now */

void usbssp_dbg_regs(struct usbssp_udc *usbssp_data)
{
	u32 temp;

	usbssp_dbg(usbssp_data, "// USBSSP capability registers at %p:\n",
		usbssp_data->cap_regs);
	temp = readl(&usbssp_data->cap_regs->hc_capbase);
	usbssp_dbg(usbssp_data, "// @%p = 0x%x (CAPLENGTH AND HCIVERSION)\n",
		&usbssp_data->cap_regs->hc_capbase, temp);
	usbssp_dbg(usbssp_data, "//   CAPLENGTH: 0x%x\n",
		(unsigned int) HC_LENGTH(temp));
	usbssp_dbg(usbssp_data, "//   HCIVERSION: 0x%x\n",
		(unsigned int) HC_VERSION(temp));

	usbssp_dbg(usbssp_data, "// USBSSP operational registers at %p:\n",
		usbssp_data->op_regs);

	temp = readl(&usbssp_data->cap_regs->run_regs_off);
	usbssp_dbg(usbssp_data, "// @%p = 0x%x RTSOFF\n",
		&usbssp_data->cap_regs->run_regs_off,
		(unsigned int) temp & RTSOFF_MASK);
	usbssp_dbg(usbssp_data, "// USBSSP runtime registers at %p:\n",
		usbssp_data->run_regs);

	temp = readl(&usbssp_data->cap_regs->db_off);
	usbssp_dbg(usbssp_data, "// @%p = 0x%x DBOFF\n",
		&usbssp_data->cap_regs->db_off, temp);
	usbssp_dbg(usbssp_data, "// Doorbell array at %p:\n", usbssp_data->dba);
}

static void usbssp_print_cap_regs(struct usbssp_udc *usbssp_data)
{
	u32 temp;
	u32 hci_version;

	usbssp_dbg(usbssp_data, "USBSSP capability registers at %p:\n",
		usbssp_data->cap_regs);

	temp = readl(&usbssp_data->cap_regs->hc_capbase);
	hci_version = HC_VERSION(temp);
	usbssp_dbg(usbssp_data, "CAPLENGTH AND HCIVERSION 0x%x:\n",
		(unsigned int) temp);
	usbssp_dbg(usbssp_data, "CAPLENGTH: 0x%x\n",
		(unsigned int) HC_LENGTH(temp));
	usbssp_dbg(usbssp_data, "HCIVERSION: 0x%x\n", hci_version);

	temp = readl(&usbssp_data->cap_regs->hcs_params1);
	usbssp_dbg(usbssp_data, "HCSPARAMS 1: 0x%x\n",
		(unsigned int) temp);
	usbssp_dbg(usbssp_data, "  Max device slots: %u\n",
		(unsigned int) HCS_MAX_SLOTS(temp));
	usbssp_dbg(usbssp_data, "  Max interrupters: %u\n",
		(unsigned int) HCS_MAX_INTRS(temp));
	usbssp_dbg(usbssp_data, "  Max ports: %u\n",
		(unsigned int) HCS_MAX_PORTS(temp));

	temp = readl(&usbssp_data->cap_regs->hcs_params2);
	usbssp_dbg(usbssp_data, "HCSPARAMS 2: 0x%x\n",
		(unsigned int) temp);
	usbssp_dbg(usbssp_data, " Isoc scheduling threshold: %u\n",
		(unsigned int) HCS_IST(temp));
	usbssp_dbg(usbssp_data, " Maximum allowed segments in event ring: %u\n",
		(unsigned int) HCS_ERST_MAX(temp));

	temp = readl(&usbssp_data->cap_regs->hcs_params3);
	usbssp_dbg(usbssp_data, "HCSPARAMS 3 0x%x:\n",
		(unsigned int) temp);
	usbssp_dbg(usbssp_data, "  Worst case U1 device exit latency: %u\n",
		(unsigned int) HCS_U1_LATENCY(temp));
	usbssp_dbg(usbssp_data, "  Worst case U2 device exit latency: %u\n",
		(unsigned int) HCS_U2_LATENCY(temp));

	temp = readl(&usbssp_data->cap_regs->hcc_params);
	usbssp_dbg(usbssp_data, "HCC PARAMS 0x%x:\n", (unsigned int) temp);
	usbssp_dbg(usbssp_data, "  HC generates %s bit addresses\n",
		HCC_64BIT_ADDR(temp) ? "64" : "32");
	usbssp_dbg(usbssp_data, "  HC %s Contiguous Frame ID Capability\n",
		HCC_CFC(temp) ? "has" : "hasn't");
	usbssp_dbg(usbssp_data,
		"  HC %s generate Stopped - Short Package event\n",
		HCC_SPC(temp) ? "can" : "can't");

	temp = readl(&usbssp_data->cap_regs->run_regs_off);
	usbssp_dbg(usbssp_data, "RTSOFF 0x%x:\n", temp & RTSOFF_MASK);

	temp = readl(&usbssp_data->cap_regs->hcc_params2);
	usbssp_dbg(usbssp_data, "HCC PARAMS2 0x%x:\n", (unsigned int) temp);
	usbssp_dbg(usbssp_data, "  HC %s Force save context capability",
		HCC2_FSC(temp) ? "supports" : "doesn't support");
	usbssp_dbg(usbssp_data, "  HC %s Large ESIT Payload Capability",
		HCC2_LEC(temp) ? "supports" : "doesn't support");
	usbssp_dbg(usbssp_data, "  HC %s Extended TBC capability",
		HCC2_ETC(temp) ? "supports" : "doesn't support");

}

static void usbssp_print_command_reg(struct usbssp_udc *usbssp_data)
{
	u32 temp;

	temp = readl(&usbssp_data->op_regs->command);
	usbssp_dbg(usbssp_data, "USBCMD 0x%x:\n", temp);
	usbssp_dbg(usbssp_data, "  HC is %s\n",
		(temp & CMD_RUN) ? "running" : "being stopped");
	usbssp_dbg(usbssp_data, "  HC has %sfinished hard reset\n",
		(temp & CMD_RESET) ? "not " : "");
	usbssp_dbg(usbssp_data, "  Event Interrupts %s\n",
		(temp & CMD_EIE) ? "enabled " : "disabled");
	usbssp_dbg(usbssp_data, "  Host System Error Interrupts %s\n",
		(temp & CMD_HSEIE) ? "enabled " : "disabled");
}

static void usbssp_print_status(struct usbssp_udc *usbssp_data)
{
	u32 temp;

	temp = readl(&usbssp_data->op_regs->status);
	usbssp_dbg(usbssp_data, "USBSTS 0x%x:\n", temp);
	usbssp_dbg(usbssp_data, "  Event ring is %sempty\n",
		(temp & STS_EINT) ? "not " : "");
	usbssp_dbg(usbssp_data, "  %sHost System Error\n",
		(temp & STS_FATAL) ? "WARNING: " : "No ");
	usbssp_dbg(usbssp_data, "  HC is %s\n",
		(temp & STS_HALT) ? "halted" : "running");
}

static void usbssp_print_op_regs(struct usbssp_udc *usbssp_data)
{
	usbssp_dbg(usbssp_data, "USBSSP operational registers at %p:\n",
		usbssp_data->op_regs);
	usbssp_print_command_reg(usbssp_data);
	usbssp_print_status(usbssp_data);
}

/*Device has only one port*/
static void usbssp_print_ports(struct usbssp_udc *usbssp_data)
{
	__le32 __iomem *addr;
	int i, j;
	int ports;
	char *names[NUM_PORT_REGS] = {
		"status",
		"power",
		"link",
		"reserved",
	};

	ports = HCS_MAX_PORTS(usbssp_data->hcs_params1);
	addr = &usbssp_data->op_regs->port_status_base;
	for (i = 0; i < ports; i++) {
		for (j = 0; j < NUM_PORT_REGS; ++j) {
			usbssp_dbg(usbssp_data, "%p port %s reg = 0x%x\n",
					addr, names[j],
					(unsigned int) readl(addr));
			addr++;
		}
	}
}

void usbssp_print_ir_set(struct usbssp_udc *usbssp_data, int set_num)
{
	struct usbssp_intr_reg __iomem *ir_set =
				&usbssp_data->run_regs->ir_set[set_num];
	void __iomem *addr;
	u32 temp;
	u64 temp_64;

	addr = &ir_set->irq_pending;
	temp = readl(addr);
	if (temp == USBSSP_INIT_VALUE)
		return;

	usbssp_dbg(usbssp_data, "  %p: ir_set[%i]\n", ir_set, set_num);

	usbssp_dbg(usbssp_data, "  %p: ir_set.pending = 0x%x\n", addr,
		  (unsigned int)temp);

	addr = &ir_set->irq_control;
	temp = readl(addr);
	usbssp_dbg(usbssp_data, "  %p: ir_set.control = 0x%x\n", addr,
		(unsigned int)temp);

	addr = &ir_set->erst_size;
	temp = readl(addr);
	usbssp_dbg(usbssp_data, "  %p: ir_set.erst_size = 0x%x\n", addr,
		(unsigned int)temp);

	addr = &ir_set->rsvd;
	temp = readl(addr);
	if (temp != USBSSP_INIT_VALUE)
		usbssp_dbg(usbssp_data, "  WARN: %p: ir_set.rsvd = 0x%x\n",
			addr, (unsigned int)temp);

	addr = &ir_set->erst_base;
	temp_64 = usbssp_read_64(usbssp_data, addr);
	usbssp_dbg(usbssp_data, "  %p: ir_set.erst_base = @%08llx\n",
		addr, temp_64);

	addr = &ir_set->erst_dequeue;
	temp_64 = usbssp_read_64(usbssp_data, addr);
	usbssp_dbg(usbssp_data, "  %p: ir_set.erst_dequeue = @%08llx\n",
		addr, temp_64);
}

void usbssp_print_run_regs(struct usbssp_udc *usbssp_data)
{
	u32 temp;
	int i;

	usbssp_dbg(usbssp_data, "USBSSP runtime registers at %p:\n",
		usbssp_data->run_regs);
	temp = readl(&usbssp_data->run_regs->microframe_index);
	usbssp_dbg(usbssp_data, "  %p: Microframe index = 0x%x\n",
		&usbssp_data->run_regs->microframe_index,
		(unsigned int) temp);
	for (i = 0; i < 7; ++i) {
		temp = readl(&usbssp_data->run_regs->rsvd[i]);
		if (temp != USBSSP_INIT_VALUE)
			usbssp_dbg(usbssp_data, "  WARN: %p: Rsvd[%i] = 0x%x\n",
				&usbssp_data->run_regs->rsvd[i],
				i, (unsigned int) temp);
	}
}

void usbssp_print_registers(struct usbssp_udc *usbssp_data)
{
	usbssp_print_cap_regs(usbssp_data);
	usbssp_print_op_regs(usbssp_data);
	usbssp_print_ports(usbssp_data);
}

void usbssp_print_trb_offsets(struct usbssp_udc *usbssp_data,
			      union usbssp_trb *trb)
{
	int i;

	for (i = 0; i < 4; ++i)
		usbssp_dbg(usbssp_data, "Offset 0x%x = 0x%x\n",
			i*4, trb->generic.field[i]);
}

/**
 * Debug a transfer request block (TRB).
 */
void usbssp_debug_trb(struct usbssp_udc *usbssp_data, union usbssp_trb *trb)
{
	u64	address;
	u32	type = le32_to_cpu(trb->link.control) & TRB_TYPE_BITMASK;

	switch (type) {
	case TRB_TYPE(TRB_LINK):
		usbssp_dbg(usbssp_data, "Link TRB:\n");
		usbssp_print_trb_offsets(usbssp_data, trb);

		address = le64_to_cpu(trb->link.segment_ptr);
		usbssp_dbg(usbssp_data,
			"Next ring segment DMA address = 0x%llx\n", address);

		usbssp_dbg(usbssp_data, "Interrupter target = 0x%x\n",
			GET_INTR_TARGET(le32_to_cpu(trb->link.intr_target)));
		usbssp_dbg(usbssp_data, "Cycle bit = %u\n",
			le32_to_cpu(trb->link.control) & TRB_CYCLE);
		usbssp_dbg(usbssp_data, "Toggle cycle bit = %u\n",
			le32_to_cpu(trb->link.control) & LINK_TOGGLE);
		usbssp_dbg(usbssp_data, "No Snoop bit = %u\n",
			le32_to_cpu(trb->link.control) & TRB_NO_SNOOP);
		break;
	case TRB_TYPE(TRB_TRANSFER):
		address = le64_to_cpu(trb->trans_event.buffer);
		usbssp_dbg(usbssp_data,
			"DMA address or buffer contents= %llu\n", address);
		break;
	case TRB_TYPE(TRB_COMPLETION):
		address = le64_to_cpu(trb->event_cmd.cmd_trb);
		usbssp_dbg(usbssp_data, "Command TRB pointer = %llu\n",
			address);
		usbssp_dbg(usbssp_data, "Completion status = %u\n",
			GET_COMP_CODE(le32_to_cpu(trb->event_cmd.status)));
		usbssp_dbg(usbssp_data, "Flags = 0x%x\n",
			le32_to_cpu(trb->event_cmd.flags));
		break;
	default:
		usbssp_dbg(usbssp_data, "Unknown TRB with TRB type ID %u\n",
				(unsigned int) type>>10);
		usbssp_print_trb_offsets(usbssp_data, trb);
		break;
	}
}

/**
 * Debug a segment with an ring.
 *
 * @return The Link TRB of the segment, or NULL if there is no Link TRB
 * (which is a bug, since all segments must have a Link TRB).
 *
 * Prints out all TRBs in the segment, even those after the Link TRB.
 *
 */
void usbssp_debug_segment(struct usbssp_udc *usbssp_data,
			  struct usbssp_segment *seg)
{
	int i;
	u64 addr = seg->dma;
	union usbssp_trb *trb = seg->trbs;

	for (i = 0; i < TRBS_PER_SEGMENT; ++i) {
		trb = &seg->trbs[i];
		usbssp_dbg(usbssp_data, "@%016llx %08x %08x %08x %08x\n", addr,
			lower_32_bits(le64_to_cpu(trb->link.segment_ptr)),
			upper_32_bits(le64_to_cpu(trb->link.segment_ptr)),
			le32_to_cpu(trb->link.intr_target),
			le32_to_cpu(trb->link.control));
		addr += sizeof(*trb);
	}
}

void usbssp_dbg_ring_ptrs(struct usbssp_udc *usbssp_data,
			  struct usbssp_ring *ring)
{
	usbssp_dbg(usbssp_data, "Ring deq = %p (virt), 0x%llx (dma)\n",
		ring->dequeue,
		(unsigned long long)usbssp_trb_virt_to_dma(ring->deq_seg,
		ring->dequeue));
	usbssp_dbg(usbssp_data, "Ring enq = %p (virt), 0x%llx (dma)\n",
		ring->enqueue,
		(unsigned long long)usbssp_trb_virt_to_dma(ring->enq_seg,
		ring->enqueue));
}

/**
 * Debugging for an USBSSP ring, which is a queue broken into multiple segments.
 *
 * Print out each segment in the ring.  Check that the DMA address in
 * each link segment actually matches the segment's stored DMA address.
 * Check that the link end bit is only set at the end of the ring.
 * Check that the dequeue and enqueue pointers point to real data in this ring
 * (not some other ring).
 */
void usbssp_debug_ring(struct usbssp_udc *usbssp_data, struct usbssp_ring *ring)
{
	struct usbssp_segment *seg;
	struct usbssp_segment *first_seg = ring->first_seg;

	usbssp_debug_segment(usbssp_data, first_seg);

	for (seg = first_seg->next; seg != first_seg; seg = seg->next)
		usbssp_debug_segment(usbssp_data, seg);
}

void usbssp_dbg_ep_rings(struct usbssp_udc *usbssp_data,
			 unsigned int ep_index, struct usbssp_ep *ep)
{
	int i;
	struct usbssp_ring *ring;

	if (ep->ep_state & EP_HAS_STREAMS) {
		for (i = 1; i < ep->stream_info->num_streams; i++) {
			ring = ep->stream_info->stream_rings[i];
			usbssp_dbg(usbssp_data,
				"Dev %d endpoint %d stream ID %d:\n",
				usbssp_data->slot_id, ep_index, i);
			usbssp_debug_segment(usbssp_data, ring->deq_seg);
		}
	} else {
		ring = ep->ring;
		if (!ring)
			return;
		usbssp_dbg(usbssp_data, "Dev %d endpoint ring %d:\n",
			usbssp_data->slot_id, ep_index);
		usbssp_debug_segment(usbssp_data, ring->deq_seg);
	}
}

void usbssp_dbg_erst(struct usbssp_udc *usbssp_data, struct usbssp_erst *erst)
{
	u64 addr = erst->erst_dma_addr;
	int i;
	struct usbssp_erst_entry *entry;

	for (i = 0; i < erst->num_entries; ++i) {
		entry = &erst->entries[i];
		usbssp_dbg(usbssp_data, "@%016llx %08x %08x %08x %08x\n",
			addr, lower_32_bits(le64_to_cpu(entry->seg_addr)),
			upper_32_bits(le64_to_cpu(entry->seg_addr)),
			le32_to_cpu(entry->seg_size), le32_to_cpu(entry->rsvd));
		addr += sizeof(*entry);
	}
}

void usbssp_dbg_cmd_ptrs(struct usbssp_udc *usbssp_data)
{
	u64 val;

	val = usbssp_read_64(usbssp_data, &usbssp_data->op_regs->cmd_ring);
	usbssp_dbg(usbssp_data,
		"// USBSSP command ring deq ptr low bits + flags = @%08x\n",
		lower_32_bits(val));
	usbssp_dbg(usbssp_data,
		"// USBSSP command ring deq ptr high bits = @%08x\n",
		upper_32_bits(val));
}

/* Print the last 32 bytes for 64-byte contexts */
static void dbg_rsvd64(struct usbssp_udc *usbssp_data, u64 *ctx, dma_addr_t dma)
{
	int i;

	for (i = 0; i < 4; ++i) {
		usbssp_dbg(usbssp_data,
			"@%p (virt) @%08llx (dma) %#08llx - rsvd64[%d]\n",
			&ctx[4 + i], (unsigned long long)dma,
			ctx[4 + i], i);
		dma += 8;
	}
}

char *usbssp_get_slot_state(struct usbssp_udc *usbssp_data,
		struct usbssp_container_ctx *ctx)
{
	struct usbssp_slot_ctx *slot_ctx =
			usbssp_get_slot_ctx(usbssp_data, ctx);

	switch (GET_SLOT_STATE(le32_to_cpu(slot_ctx->dev_state))) {
	case SLOT_STATE_ENABLED:
		return "enabled/disabled";
	case SLOT_STATE_DEFAULT:
		return "default";
	case SLOT_STATE_ADDRESSED:
		return "addressed";
	case SLOT_STATE_CONFIGURED:
		return "configured";
	default:
		return "reserved";
	}
}

static void usbssp_dbg_slot_ctx(struct usbssp_udc *usbssp_data,
				struct usbssp_container_ctx *ctx)
{
	/* Fields are 32 bits wide, DMA addresses are in bytes */
	int field_size = 32 / 8;
	int i;

	struct usbssp_slot_ctx *slot_ctx =
			usbssp_get_slot_ctx(usbssp_data, ctx);
	dma_addr_t dma = ctx->dma +
		((unsigned long)slot_ctx - (unsigned long)ctx->bytes);
	int csz = HCC_64BYTE_CONTEXT(usbssp_data->hcc_params);

	usbssp_dbg(usbssp_data, "Slot Context:\n");
	usbssp_dbg(usbssp_data, "@%p (virt) @%08llx (dma) %#08x - dev_info\n",
		&slot_ctx->dev_info,
		(unsigned long long)dma, slot_ctx->dev_info);
	dma += field_size;
	usbssp_dbg(usbssp_data, "@%p (virt) @%08llx (dma) %#08x - dev_info2\n",
		&slot_ctx->dev_info2,
		(unsigned long long)dma, slot_ctx->dev_info2);
	dma += field_size;
	usbssp_dbg(usbssp_data, "@%p (virt) @%08llx (dma) %#08x - int_target\n",
		&slot_ctx->int_target,
		(unsigned long long)dma, slot_ctx->int_target);
	dma += field_size;
	usbssp_dbg(usbssp_data, "@%p (virt) @%08llx (dma) %#08x - dev_state\n",
		&slot_ctx->dev_state,
		(unsigned long long)dma, slot_ctx->dev_state);
	dma += field_size;
	for (i = 0; i < 4; ++i) {
		usbssp_dbg(usbssp_data,
			"@%p (virt) @%08llx (dma) %#08x - rsvd[%d]\n",
			&slot_ctx->reserved[i], (unsigned long long)dma,
			slot_ctx->reserved[i], i);
		dma += field_size;
	}

	if (csz)
		dbg_rsvd64(usbssp_data, (u64 *)slot_ctx, dma);
}

static void usbssp_dbg_ep_ctx(struct usbssp_udc *usbssp_data,
		     struct usbssp_container_ctx *ctx,
		     unsigned int last_ep)
{
	int i, j;
	int last_ep_ctx = 31;
	/* Fields are 32 bits wide, DMA addresses are in bytes */
	int field_size = 32 / 8;
	int csz = HCC_64BYTE_CONTEXT(usbssp_data->hcc_params);

	if (last_ep < 31)
		last_ep_ctx = last_ep + 1;
	for (i = 0; i < last_ep_ctx; ++i) {
		unsigned int epaddr = usbssp_get_endpoint_address(i);
		struct usbssp_ep_ctx *ep_ctx = usbssp_get_ep_ctx(usbssp_data,
								 ctx, i);
		dma_addr_t dma = ctx->dma +
			((unsigned long)ep_ctx - (unsigned long)ctx->bytes);

		usbssp_dbg(usbssp_data,
			"%s Endpoint %02d Context (ep_index %02d):\n",
			usb_endpoint_out(epaddr) ? "OUT" : "IN",
			epaddr & USB_ENDPOINT_NUMBER_MASK, i);
		usbssp_dbg(usbssp_data,
			"@%p (virt) @%08llx (dma) %#08x - ep_info\n",
			&ep_ctx->ep_info,
			(unsigned long long)dma, ep_ctx->ep_info);
		dma += field_size;
		usbssp_dbg(usbssp_data,
			"@%p (virt) @%08llx (dma) %#08x - ep_info2\n",
			&ep_ctx->ep_info2,
			(unsigned long long)dma, ep_ctx->ep_info2);
		dma += field_size;
		usbssp_dbg(usbssp_data,
			"@%p (virt) @%08llx (dma) %#08llx - deq\n",
			&ep_ctx->deq,
			(unsigned long long)dma, ep_ctx->deq);
		dma += 2*field_size;
		usbssp_dbg(usbssp_data,
			"@%p (virt) @%08llx (dma) %#08x - tx_info\n",
			&ep_ctx->tx_info,
			(unsigned long long)dma, ep_ctx->tx_info);
		dma += field_size;
		for (j = 0; j < 3; ++j) {
			usbssp_dbg(usbssp_data,
				"@%p (virt) @%08llx (dma) %#08x - rsvd[%d]\n",
				&ep_ctx->reserved[j],
				(unsigned long long)dma,
				ep_ctx->reserved[j], j);
			dma += field_size;
		}

		if (csz)
			dbg_rsvd64(usbssp_data, (u64 *)ep_ctx, dma);
	}
}

void usbssp_dbg_ctx(struct usbssp_udc *usbssp_data,
		  struct usbssp_container_ctx *ctx,
		  unsigned int last_ep)
{
	int i;
	/* Fields are 32 bits wide, DMA addresses are in bytes */
	int field_size = 32 / 8;
	dma_addr_t dma = ctx->dma;
	int csz = HCC_64BYTE_CONTEXT(usbssp_data->hcc_params);

	if (ctx->type == USBSSP_CTX_TYPE_INPUT) {
		struct usbssp_input_control_ctx *ctrl_ctx =
			usbssp_get_input_control_ctx(ctx);
		if (!ctrl_ctx) {
			usbssp_warn(usbssp_data,
				"Could not get input context, bad type.\n");
			return;
		}

		usbssp_dbg(usbssp_data,
			"@%p (virt) @%08llx (dma) %#08x - drop flags\n",
			&ctrl_ctx->drop_flags, (unsigned long long)dma,
			ctrl_ctx->drop_flags);
		dma += field_size;
		usbssp_dbg(usbssp_data,
			"@%p (virt) @%08llx (dma) %#08x - add flags\n",
			&ctrl_ctx->add_flags, (unsigned long long)dma,
			ctrl_ctx->add_flags);
		dma += field_size;
		for (i = 0; i < 6; ++i) {
			usbssp_dbg(usbssp_data,
				"@%p (virt) @%08llx (dma) %#08x - rsvd2[%d]\n",
				&ctrl_ctx->rsvd2[i], (unsigned long long)dma,
				ctrl_ctx->rsvd2[i], i);
			dma += field_size;
		}

		if (csz)
			dbg_rsvd64(usbssp_data, (u64 *)ctrl_ctx, dma);
	}

	usbssp_dbg_slot_ctx(usbssp_data, ctx);
	usbssp_dbg_ep_ctx(usbssp_data, ctx, last_ep);
}

void usbssp_dbg_trace(struct usbssp_udc *usbssp_data,
		      void (*trace)(struct va_format *),
		      const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	usbssp_dbg(usbssp_data, "%pV\n", &vaf);
	trace(&vaf);
	va_end(args);
}
EXPORT_SYMBOL_GPL(usbssp_dbg_trace);
