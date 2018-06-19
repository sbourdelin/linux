/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Linux-specific definitions for managing interactions with Microsoft's
 * Hyper-V hypervisor. Definitions that are specified in the Hyper-V
 * Top Level Functional Spec (TLFS) should not go in this file, but
 * should instead go in hyperv-tlfs.h.
 *
 * Copyright (C) 2018, Microsoft, Inc.
 *
 * Author : Michael Kelley <mikelley@microsoft.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 */

#ifndef _ASM_ARM64_MSHYPERV_H
#define _ASM_ARM64_MSHYPERV_H

#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/clocksource.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <asm/hyperv-tlfs.h>

/*
 * Hyper-V always runs with a page size of 4096. These definitions
 * are used when communicating with Hyper-V using guest physical
 * pages and guest physical page addresses, since the guest page
 * size may not be 4096 on ARM64.
 */
#define HV_HYP_PAGE_SIZE	4096
#define HV_HYP_PAGE_SHIFT	12
#define HV_HYP_PAGE_MASK	(~(HV_HYP_PAGE_SIZE - 1))


struct ms_hyperv_info {
	u32 features;
	u32 misc_features;
	u32 hints;
	u32 max_vp_index;
	u32 max_lp_index;
};
extern struct ms_hyperv_info ms_hyperv;

/*
 * Define the IRQ numbers/vectors used by Hyper-V VMbus interrupts
 * and by STIMER0 Direct Mode interrupts. Hyper-V should be supplying
 * these values through ACPI, but there are no other interrupting
 * devices in a Hyper-V VM on ARM64, so it's OK to hard code for now.
 * The "CALLBACK_VECTOR" terminology is a left-over from the x86/x64
 * world that is used in architecture independent Hyper-V code.
 */
#define HYPERVISOR_CALLBACK_VECTOR 16
#define	HV_STIMER0_IRQNR	   17

extern u64 hv_do_hypercall(u64 control, void *inputaddr, void *outputaddr);
extern u64 hv_do_fast_hypercall8(u16 control, u64 input8);

extern u64 hv_do_hvc(u64 control, ...);
extern u64 hv_do_hvc_fast_get(u64 control, u64 input1, u64 input2, u64 input3,
		struct hv_get_vp_register_output *output);

/*
 * Declare calls to get and set Hyper-V VP register values on ARM64, which
 * requires a hypercall.
 */
extern void hv_set_vpreg(u32 reg, u64 value);
extern u64 hv_get_vpreg(u32 reg);
extern void hv_get_vpreg_128(u32 reg, struct hv_get_vp_register_output *result);

/*
 * The guest OS needs to register the guest ID with the hypervisor.
 * The guest ID is a 64 bit entity and the structure of this ID is
 * specified in the Hyper-V specification:
 *
 * msdn.microsoft.com/en-us/library/windows/hardware/ff542653%28v=vs.85%29.aspx
 *
 * While the current guideline does not specify how Linux guest ID(s)
 * need to be generated, our plan is to publish the guidelines for
 * Linux and other guest operating systems that currently are hosted
 * on Hyper-V. The implementation here conforms to this yet
 * unpublished guidelines.
 *
 *
 * Bit(s)
 * 63 - Indicates if the OS is Open Source or not; 1 is Open Source
 * 62:56 - Os Type; Linux is 0x100
 * 55:48 - Distro specific identification
 * 47:16 - Linux kernel version number
 * 15:0  - Distro specific identification
 *
 * Generate the guest ID based on the guideline described above.
 */

static inline  __u64 generate_guest_id(__u64 d_info1, __u64 kernel_version,
				       __u64 d_info2)
{
	__u64 guest_id = 0;

	guest_id = (((__u64)HV_LINUX_VENDOR_ID) << 48);
	guest_id |= (d_info1 << 48);
	guest_id |= (kernel_version << 16);
	guest_id |= d_info2;

	return guest_id;
}


/* Free the message slot and signal end-of-message if required */
static inline void vmbus_signal_eom(struct hv_message *msg, u32 old_msg_type)
{
	/*
	 * On crash we're reading some other CPU's message page and we need
	 * to be careful: this other CPU may already had cleared the header
	 * and the host may already had delivered some other message there.
	 * In case we blindly write msg->header.message_type we're going
	 * to lose it. We can still lose a message of the same type but
	 * we count on the fact that there can only be one
	 * CHANNELMSG_UNLOAD_RESPONSE and we don't care about other messages
	 * on crash.
	 */
	if (cmpxchg(&msg->header.message_type, old_msg_type,
		    HVMSG_NONE) != old_msg_type)
		return;

	/*
	 * Make sure the write to MessageType (ie set to
	 * HVMSG_NONE) happens before we read the
	 * MessagePending and EOMing. Otherwise, the EOMing
	 * will not deliver any more messages since there is
	 * no empty slot
	 */
	mb();

	if (msg->header.message_flags.msg_pending) {
		/*
		 * This will cause message queue rescan to
		 * possibly deliver another msg from the
		 * hypervisor
		 */
		hv_set_vpreg(HvRegisterEom, 0);
	}
}

/*
 * Use the Hyper-V provided stimer0 as the timer that is made
 * available to the architecture independent Hyper-V drivers.
 */
#define hv_init_timer(timer, tick) \
		hv_set_vpreg(HvRegisterStimer0Count + (2*timer), tick)
#define hv_init_timer_config(timer, val) \
		hv_set_vpreg(HvRegisterStimer0Config + (2*timer), val)
#define hv_get_current_tick(tick) \
		(tick = hv_get_vpreg(HvRegisterTimeRefCount))

#define hv_get_simp(val) (val = hv_get_vpreg(HvRegisterSipp))
#define hv_set_simp(val) hv_set_vpreg(HvRegisterSipp, val)

#define hv_get_siefp(val) (val = hv_get_vpreg(HvRegisterSifp))
#define hv_set_siefp(val) hv_set_vpreg(HvRegisterSifp, val)

#define hv_get_synic_state(val) (val = hv_get_vpreg(HvRegisterScontrol))
#define hv_set_synic_state(val) hv_set_vpreg(HvRegisterScontrol, val)

#define hv_get_vp_index(index) (index = hv_get_vpreg(HvRegisterVpIndex))

/*
 * Hyper-V SINT registers are numbered sequentially, so we can just
 * add the SINT number to the register number of SINT0
 */
#define hv_get_synint_state(sint_num, val) \
		(val = hv_get_vpreg(HvRegisterSint0 + sint_num))
#define hv_set_synint_state(sint_num, val) \
		hv_set_vpreg(HvRegisterSint0 + sint_num, val)

void hv_setup_vmbus_irq(void (*handler)(void));
void hv_remove_vmbus_irq(void);
void hv_enable_vmbus_irq(void);
void hv_disable_vmbus_irq(void);

void hv_setup_kexec_handler(void (*handler)(void));
void hv_remove_kexec_handler(void);
void hv_setup_crash_handler(void (*handler)(struct pt_regs *regs));
void hv_remove_crash_handler(void);

#if IS_ENABLED(CONFIG_HYPERV)
extern struct clocksource *hyperv_cs;

/*
 * Hypervisor's notion of virtual processor ID is different from
 * Linux' notion of CPU ID. This information can only be retrieved
 * in the context of the calling CPU. Setup a map for easy access
 * to this information.
 */
extern u32 *hv_vp_index;
extern u32 hv_max_vp_index;

/**
 * hv_cpu_number_to_vp_number() - Map CPU to VP.
 * @cpu_number: CPU number in Linux terms
 *
 * This function returns the mapping between the Linux processor
 * number and the hypervisor's virtual processor number, useful
 * in making hypercalls and such that talk about specific
 * processors.
 *
 * Return: Virtual processor number in Hyper-V terms
 */
static inline int hv_cpu_number_to_vp_number(int cpu_number)
{
	return hv_vp_index[cpu_number];
}

void hyperv_report_panic(struct pt_regs *regs, long err);
bool hv_is_hyperv_initialized(void);
void hyperv_cleanup(void);
#else /* CONFIG_HYPERV */
static inline bool hv_is_hyperv_initialized(void) { return false; }
static inline void hyperv_cleanup(void) {}
#endif /* CONFIG_HYPERV */

#if IS_ENABLED(CONFIG_HYPERV)
#define hv_enable_stimer0_percpu_irq(irq)	enable_percpu_irq(irq, 0)
#define hv_disable_stimer0_percpu_irq(irq)	disable_percpu_irq(irq)
extern int hv_setup_stimer0_irq(int *irq, int *vector, void (*handler)(void));
extern void hv_remove_stimer0_irq(int irq);
#endif

extern struct ms_hyperv_tsc_page *hv_get_tsc_page(void);
static inline u64 hv_read_tsc_page_tsc(const struct ms_hyperv_tsc_page *tsc_pg,
				       u64 *cur_tsc)
{
	u64	scale, offset;
	u32	sequence;

	/*
	 * The protocol for reading Hyper-V TSC page is specified in Hypervisor
	 * Top-Level Functional Specification.  To get the reference time we
	 * must do the following:
	 * - READ ReferenceTscSequence
	 *   A special '0' value indicates the time source is unreliable and we
	 *   need to use something else.
	 * - ReferenceTime =
	 *        ((CNTVCT_EL0) * ReferenceTscScale) >> 64) + ReferenceTscOffset
	 * - READ ReferenceTscSequence again. In case its value has changed
	 *   since our first reading we need to discard ReferenceTime and repeat
	 *   the whole sequence as the hypervisor was updating the page in
	 *   between.
	 */
	do {
		sequence = READ_ONCE(tsc_pg->tsc_sequence);
		/*
		 * Make sure we read sequence before we read other values from
		 * TSC page.
		 */
		smp_rmb();

		scale = READ_ONCE(tsc_pg->tsc_scale);
		offset = READ_ONCE(tsc_pg->tsc_offset);
		isb();
		*cur_tsc = read_sysreg(cntvct_el0);
		isb();

		/*
		 * Make sure we read sequence after we read all other values
		 * from TSC page.
		 */
		smp_rmb();

	} while (READ_ONCE(tsc_pg->tsc_sequence) != sequence);

	return mul_u64_u64_shr(*cur_tsc, scale, 64) + offset;
}

static inline u64 hv_read_tsc_page(const struct ms_hyperv_tsc_page *tsc_pg)
{
	u64 cur_tsc;

	return hv_read_tsc_page_tsc(tsc_pg, &cur_tsc);
}

#endif
