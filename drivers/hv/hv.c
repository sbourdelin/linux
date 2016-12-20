/*
 * Copyright (c) 2009, Microsoft Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 * Authors:
 *   Haiyang Zhang <haiyangz@microsoft.com>
 *   Hank Janssen  <hjanssen@microsoft.com>
 *
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/hyperv.h>
#include <linux/version.h>
#include <linux/interrupt.h>
#include <linux/clockchips.h>
#include <asm/cacheflush.h>
#include <asm/hyperv.h>
#include <asm/mshyperv.h>
#include "hyperv_vmbus.h"

/* The one and only */
struct hv_context hv_context = {
	.synic_initialized	= false,
	.hypercall_page		= NULL,
};

#define HV_TIMER_FREQUENCY (10 * 1000 * 1000) /* 100ns period */
#define HV_MAX_MAX_DELTA_TICKS 0xffffffff
#define HV_MIN_DELTA_TICKS 1

/*
 * The guest OS needs to register the guest ID with the hypervisor.
 * The guest ID is a 64 bit entity and the structure of this ID is
 * specified in the Hyper-V specification:
 *
 * http://msdn.microsoft.com/en-us/library/windows/hardware/ff542653%28v=vs.85%29.aspx
 *
 * While the current guideline does not specify how Linux guest ID(s)
 * need to be generated, our plan is to publish the guidelines for
 * Linux and other guest operating systems that currently are hosted
 * on Hyper-V. The implementation here conforms to this yet
 * unpublished guidelines.
 *
 * Bit(s)
 * 63 - Indicates if the OS is Open Source or not; 1 is Open Source
 * 62:56 - Os Type; Linux is 0x100
 * 55:48 - Distro specific identification
 * 47:16 - Linux kernel version number
 * 15:0  - Distro specific identification
 */

#define HV_LINUX_VENDOR_ID		0x8100

/*
 * Generate the guest ID based on the guideline described above.
 */

static u64 generate_guest_id(u8 d_info1, u32 kernel_version, u16 d_info2)
{
	u64 guest_id;

	guest_id = ((u64)HV_LINUX_VENDOR_ID) << 48;
	guest_id |= ((u64)d_info1) << 48;
	guest_id |= ((u64)kernel_version) << 16;
	guest_id |= (u64)d_info2;

	return guest_id;
}

/*
 * query_hypervisor_info - Get version info of the windows hypervisor
 */
unsigned int host_info_eax;
unsigned int host_info_ebx;
unsigned int host_info_ecx;
unsigned int host_info_edx;

static int query_hypervisor_info(void)
{
	unsigned int eax;
	unsigned int ebx;
	unsigned int ecx;
	unsigned int edx;
	unsigned int max_leaf;
	unsigned int op;

	/*
	* Its assumed that this is called after confirming that Viridian
	* is present. Query id and revision.
	*/
	eax = 0;
	ebx = 0;
	ecx = 0;
	edx = 0;
	op = HYPERV_CPUID_VENDOR_AND_MAX_FUNCTIONS;
	cpuid(op, &eax, &ebx, &ecx, &edx);

	max_leaf = eax;

	if (max_leaf >= HYPERV_CPUID_VERSION) {
		eax = 0;
		ebx = 0;
		ecx = 0;
		edx = 0;
		op = HYPERV_CPUID_VERSION;
		cpuid(op, &eax, &ebx, &ecx, &edx);
		host_info_eax = eax;
		host_info_ebx = ebx;
		host_info_ecx = ecx;
		host_info_edx = edx;
	}
	return max_leaf;
}

/*
 * hv_do_hypercall- Invoke the specified hypercall
 */
u64 hv_do_hypercall(u64 control, void *input, void *output)
{
	u64 input_address = (input) ? virt_to_phys(input) : 0;
	u64 output_address = (output) ? virt_to_phys(output) : 0;
	void *hypercall_page = hv_context.hypercall_page;
#ifdef CONFIG_X86_64
	u64 hv_status = 0;

	if (!hypercall_page)
		return (u64)ULLONG_MAX;

	__asm__ __volatile__("mov %0, %%r8" : : "r" (output_address) : "r8");
	__asm__ __volatile__("call *%3" : "=a" (hv_status) :
			     "c" (control), "d" (input_address),
			     "m" (hypercall_page));

	return hv_status;

#else

	u32 control_hi = control >> 32;
	u32 control_lo = control & 0xFFFFFFFF;
	u32 hv_status_hi = 1;
	u32 hv_status_lo = 1;
	u32 input_address_hi = input_address >> 32;
	u32 input_address_lo = input_address & 0xFFFFFFFF;
	u32 output_address_hi = output_address >> 32;
	u32 output_address_lo = output_address & 0xFFFFFFFF;

	if (!hypercall_page)
		return (u64)ULLONG_MAX;

	__asm__ __volatile__ ("call *%8" : "=d"(hv_status_hi),
			      "=a"(hv_status_lo) : "d" (control_hi),
			      "a" (control_lo), "b" (input_address_hi),
			      "c" (input_address_lo), "D"(output_address_hi),
			      "S"(output_address_lo), "m" (hypercall_page));

	return hv_status_lo | ((u64)hv_status_hi << 32);
#endif /* !x86_64 */
}
EXPORT_SYMBOL_GPL(hv_do_hypercall);

#ifdef CONFIG_X86_64
static cycle_t read_hv_clock_tsc(struct clocksource *arg)
{
	struct hv_ref_tsc_page *tsc_pg = hv_context.tsc_page;
	u32 sequence;
	u64 scale;
	s64 offset;

	do {
		sequence = tsc_pg->tsc_sequence;
		virt_rmb();

		if (!sequence) {
			/* fallback to MSR */
			cycle_t current_tick;
			rdmsrl(HV_X64_MSR_TIME_REF_COUNT, current_tick);
			return current_tick;
		}

		scale = tsc_pg->tsc_scale;
		offset = tsc_pg->tsc_offset;

		virt_rmb();
	} while (tsc_pg->tsc_sequence != sequence);

	return mul_u64_u64_shr(rdtsc_ordered(), scale, 64) + offset;
}

static struct clocksource hyperv_cs_tsc = {
		.name           = "hyperv_clocksource_tsc_page",
		.rating         = 425,
		.read           = read_hv_clock_tsc,
		.mask           = CLOCKSOURCE_MASK(64),
		.flags          = CLOCK_SOURCE_IS_CONTINUOUS,
};
#endif


/*
 * hv_init - Main initialization routine.
 *
 * This routine must be called before any other routines in here are called
 */
int hv_init(void)
{
	int max_leaf;
	u64 hypercall_msr = 0;
	void *virtaddr = NULL;

	memset(hv_context.synic_event_page, 0, sizeof(void *) * NR_CPUS);
	memset(hv_context.synic_message_page, 0,
	       sizeof(void *) * NR_CPUS);
	memset(hv_context.post_msg_page, 0,
	       sizeof(void *) * NR_CPUS);
	memset(hv_context.vp_index, 0,
	       sizeof(int) * NR_CPUS);
	memset(hv_context.event_dpc, 0,
	       sizeof(void *) * NR_CPUS);
	memset(hv_context.msg_dpc, 0,
	       sizeof(void *) * NR_CPUS);
	memset(hv_context.clk_evt, 0,
	       sizeof(void *) * NR_CPUS);

	max_leaf = query_hypervisor_info();

	/*
	 * Write our OS ID.
	 */
	hv_context.guestid = generate_guest_id(0, LINUX_VERSION_CODE, 0);
	wrmsrl(HV_X64_MSR_GUEST_OS_ID, hv_context.guestid);

	virtaddr = (void *)get_zeroed_page(GFP_KERNEL);
	if (!virtaddr || set_memory_x((unsigned long)virtaddr, 1))
		goto cleanup;
	hv_context.hypercall_page = virtaddr;

	rdmsrl(HV_X64_MSR_HYPERCALL, hypercall_msr);
	hypercall_msr &= PAGE_MASK;
	hypercall_msr = HV_X64_MSR_HYPERCALL_ENABLE | virt_to_phys(virtaddr);
	wrmsrl(HV_X64_MSR_HYPERCALL, hypercall_msr);

	/* Confirm that hypercall page did get setup. */
	rdmsrl(HV_X64_MSR_HYPERCALL, hypercall_msr);
	if (!(hypercall_msr & HV_X64_MSR_HYPERCALL_ENABLE))
		goto cleanup;

#ifdef CONFIG_X86_64
	if (ms_hyperv.features & HV_X64_MSR_REFERENCE_TSC_AVAILABLE) {
		u64 tsc_msr;
		void *va_tsc;

		va_tsc = (void *)get_zeroed_page(GFP_KERNEL);
		if (!va_tsc)
			goto cleanup;
		hv_context.tsc_page = va_tsc;

		rdmsrl(HV_X64_MSR_REFERENCE_TSC, tsc_msr);
		tsc_msr &= PAGE_MASK;
		tsc_msr |= HV_X64_MSR_TSC_REFERENCE_ENABLE |
			virt_to_phys(va_tsc);
		wrmsrl(HV_X64_MSR_REFERENCE_TSC, tsc_msr);
		clocksource_register_hz(&hyperv_cs_tsc, NSEC_PER_SEC/100);
	}
#endif
	return 0;

cleanup:
	hypercall_msr &= ~HV_X64_MSR_HYPERCALL_ENABLE;
	wrmsrl(HV_X64_MSR_HYPERCALL, hypercall_msr);
	free_page((unsigned long)virtaddr);

	return -ENOTSUPP;
}

/*
 * hv_cleanup - Cleanup routine.
 *
 * This routine is called normally during driver unloading or exiting.
 */
void hv_cleanup(bool crash)
{
	u64 msr;

	/* Reset our OS id */
	wrmsrl(HV_X64_MSR_GUEST_OS_ID, 0);

	rdmsrl(HV_X64_MSR_HYPERCALL, msr);
	msr &= ~HV_X64_MSR_HYPERCALL_ENABLE;
	wrmsrl(HV_X64_MSR_HYPERCALL, msr);
	if (!crash)
		free_page((unsigned long)hv_context.hypercall_page);
	hv_context.hypercall_page = NULL;

#ifdef CONFIG_X86_64
	/*
	 * Cleanup the TSC page based CS.
	 */
	if (ms_hyperv.features & HV_X64_MSR_REFERENCE_TSC_AVAILABLE) {
		/*
		 * Crash can happen in an interrupt context and unregistering
		 * a clocksource is impossible and redundant in this case.
		 */
		if (!oops_in_progress) {
			clocksource_change_rating(&hyperv_cs_tsc, 10);
			clocksource_unregister(&hyperv_cs_tsc);
		}

		rdmsrl(HV_X64_MSR_REFERENCE_TSC, msr);
		msr &= ~HV_X64_MSR_TSC_REFERENCE_ENABLE;
		wrmsrl(HV_X64_MSR_REFERENCE_TSC, msr);
		if (!crash)
			free_page((unsigned long)hv_context.tsc_page);
		hv_context.tsc_page = NULL;
	}
#endif
}

/*
 * hv_post_message - Post a message using the hypervisor message IPC.
 *
 * This involves a hypercall.
 */
int hv_post_message(u32 connection_id,
		  enum hv_message_type message_type,
		  void *payload, size_t payload_size)
{

	struct hv_input_post_message *aligned_msg;
	u64 status;

	if (payload_size > HV_MESSAGE_PAYLOAD_BYTE_COUNT)
		return -EMSGSIZE;

	aligned_msg = (struct hv_input_post_message *)
			hv_context.post_msg_page[get_cpu()];

	aligned_msg->connectionid = connection_id;
	aligned_msg->reserved = 0;
	aligned_msg->message_type = message_type;
	aligned_msg->payload_size = payload_size;
	memcpy((void *)aligned_msg->payload, payload, payload_size);

	status = hv_do_hypercall(HVCALL_POST_MESSAGE, aligned_msg, NULL);

	put_cpu();
	return status & 0xFFFF;
}

static int hv_ce_set_next_event(unsigned long delta,
				struct clock_event_device *evt)
{
	cycle_t current_tick;

	WARN_ON(!clockevent_state_oneshot(evt));

	rdmsrl(HV_X64_MSR_TIME_REF_COUNT, current_tick);
	current_tick += delta;
	wrmsrl(HV_X64_MSR_STIMER0_COUNT, current_tick);
	return 0;
}

static int hv_ce_shutdown(struct clock_event_device *evt)
{
	wrmsrl(HV_X64_MSR_STIMER0_COUNT, 0);
	wrmsrl(HV_X64_MSR_STIMER0_CONFIG, 0);

	return 0;
}

static int hv_ce_set_oneshot(struct clock_event_device *evt)
{
	u64 timer_cfg = HV_STIMER_ENABLE | HV_STIMER_AUTOENABLE |
		(VMBUS_MESSAGE_SINT << 16);

	wrmsrl(HV_X64_MSR_STIMER0_CONFIG, timer_cfg);

	return 0;
}

static void hv_init_clockevent_device(struct clock_event_device *dev, int cpu)
{
	dev->name = "Hyper-V clockevent";
	dev->features = CLOCK_EVT_FEAT_ONESHOT;
	dev->cpumask = cpumask_of(cpu);
	dev->rating = 1000;
	/*
	 * Avoid settint dev->owner = THIS_MODULE deliberately as doing so will
	 * result in clockevents_config_and_register() taking additional
	 * references to the hv_vmbus module making it impossible to unload.
	 */

	dev->set_state_shutdown = hv_ce_shutdown;
	dev->set_state_oneshot = hv_ce_set_oneshot;
	dev->set_next_event = hv_ce_set_next_event;
}


int hv_synic_alloc(void)
{
	size_t size = sizeof(struct tasklet_struct);
	size_t ced_size = sizeof(struct clock_event_device);
	int cpu;

	hv_context.hv_numa_map = kzalloc(sizeof(struct cpumask) * nr_node_ids,
					 GFP_KERNEL);
	if (hv_context.hv_numa_map == NULL) {
		pr_err("Unable to allocate NUMA map\n");
		goto err;
	}

	for_each_online_cpu(cpu) {
		hv_context.event_dpc[cpu] = kmalloc(size, GFP_KERNEL);
		if (hv_context.event_dpc[cpu] == NULL) {
			pr_err("Unable to allocate event dpc\n");
			goto err;
		}
		tasklet_init(hv_context.event_dpc[cpu], vmbus_on_event, cpu);

		hv_context.msg_dpc[cpu] = kmalloc(size, GFP_KERNEL);
		if (hv_context.msg_dpc[cpu] == NULL) {
			pr_err("Unable to allocate event dpc\n");
			goto err;
		}
		tasklet_init(hv_context.msg_dpc[cpu], vmbus_on_msg_dpc, cpu);

		hv_context.clk_evt[cpu] = kzalloc(ced_size, GFP_KERNEL);
		if (hv_context.clk_evt[cpu] == NULL) {
			pr_err("Unable to allocate clock event device\n");
			goto err;
		}

		hv_init_clockevent_device(hv_context.clk_evt[cpu], cpu);

		hv_context.synic_message_page[cpu] =
			(void *)get_zeroed_page(GFP_KERNEL);

		if (hv_context.synic_message_page[cpu] == NULL) {
			pr_err("Unable to allocate SYNIC message page\n");
			goto err;
		}

		hv_context.synic_event_page[cpu] =
			(void *)get_zeroed_page(GFP_KERNEL);

		if (hv_context.synic_event_page[cpu] == NULL) {
			pr_err("Unable to allocate SYNIC event page\n");
			goto err;
		}

		hv_context.post_msg_page[cpu] =
			(void *)get_zeroed_page(GFP_KERNEL);

		if (hv_context.post_msg_page[cpu] == NULL) {
			pr_err("Unable to allocate post msg page\n");
			goto err;
		}
	}

	return 0;
err:
	return -ENOMEM;
}

static void hv_synic_free_cpu(int cpu)
{
	kfree(hv_context.event_dpc[cpu]);
	kfree(hv_context.msg_dpc[cpu]);
	kfree(hv_context.clk_evt[cpu]);
	free_page((unsigned long)hv_context.synic_event_page[cpu]);
	free_page((unsigned long)hv_context.synic_message_page[cpu]);
	free_page((unsigned long)hv_context.post_msg_page[cpu]);
}

void hv_synic_free(void)
{
	int cpu;

	kfree(hv_context.hv_numa_map);
	for_each_online_cpu(cpu)
		hv_synic_free_cpu(cpu);
}

/*
 * hv_synic_init - Initialize the Synthethic Interrupt Controller.
 *
 * If it is already initialized by another entity (ie x2v shim), we need to
 * retrieve the initialized message and event pages.  Otherwise, we create and
 * initialize the message and event pages.
 */
void hv_synic_init(void *arg)
{
	u64 msr;
	int cpu = smp_processor_id();

	if (!hv_context.hypercall_page)
		return;

	/* Check the version */
	rdmsrl(HV_X64_MSR_SVERSION, msr);

	/* Setup the Synic's message page */
	rdmsrl(HV_X64_MSR_SIMP, msr);
	msr &= PAGE_MASK;
	msr |= virt_to_phys(hv_context.synic_message_page[cpu]) |
		HV_SYNIC_SIMP_ENABLE;
	wrmsrl(HV_X64_MSR_SIMP, msr);

	/* Setup the Synic's event page */
	rdmsrl(HV_X64_MSR_SIEFP, msr);
	msr &= PAGE_MASK;
	msr |= virt_to_phys(hv_context.synic_event_page[cpu]) |
		HV_SYNIC_SIEFP_ENABLE;
	wrmsrl(HV_X64_MSR_SIEFP, msr);

	/* Setup the shared SINT. */
	rdmsrl(HV_X64_MSR_SINT0 + VMBUS_MESSAGE_SINT, msr);
	msr &= ~(HV_SYNIC_SINT_MASKED | HV_SYNIC_SINT_VECTOR_MASK);
	msr |= HYPERVISOR_CALLBACK_VECTOR | HV_SYNIC_SINT_AUTO_EOI;
	wrmsrl(HV_X64_MSR_SINT0 + VMBUS_MESSAGE_SINT, msr);

	/* Enable the global synic bit */
	rdmsrl(HV_X64_MSR_SCONTROL, msr);
	msr |= HV_SYNIC_CONTROL_ENABLE;
	wrmsrl(HV_X64_MSR_SCONTROL, msr);

	hv_context.synic_initialized = true;

	/*
	 * Setup the mapping between Hyper-V's notion
	 * of cpuid and Linux' notion of cpuid.
	 * This array will be indexed using Linux cpuid.
	 */
	rdmsrl(HV_X64_MSR_VP_INDEX, msr);
	hv_context.vp_index[cpu] = (u32)msr;

	INIT_LIST_HEAD(&hv_context.percpu_list[cpu]);

	/*
	 * Register the per-cpu clockevent source.
	 */
	if (ms_hyperv.features & HV_X64_MSR_SYNTIMER_AVAILABLE)
		clockevents_config_and_register(hv_context.clk_evt[cpu],
						HV_TIMER_FREQUENCY,
						HV_MIN_DELTA_TICKS,
						HV_MAX_MAX_DELTA_TICKS);
	return;
}

/*
 * hv_synic_clockevents_cleanup - Cleanup clockevent devices
 */
void hv_synic_clockevents_cleanup(void)
{
	int cpu;

	if (!(ms_hyperv.features & HV_X64_MSR_SYNTIMER_AVAILABLE))
		return;

	for_each_present_cpu(cpu)
		clockevents_unbind_device(hv_context.clk_evt[cpu], cpu);
}

/*
 * hv_synic_cleanup - Cleanup routine for hv_synic_init().
 */
void hv_synic_cleanup(void *arg)
{
	u64 msr;
	int cpu = smp_processor_id();

	if (!hv_context.synic_initialized)
		return;

	/* Turn off clockevent device */
	if (ms_hyperv.features & HV_X64_MSR_SYNTIMER_AVAILABLE) {
		clockevents_unbind_device(hv_context.clk_evt[cpu], cpu);
		hv_ce_shutdown(hv_context.clk_evt[cpu]);
	}

	rdmsrl(HV_X64_MSR_SINT0 + VMBUS_MESSAGE_SINT, msr);
	msr |= HV_SYNIC_SINT_MASKED;
	wrmsrl(HV_X64_MSR_SINT0 + VMBUS_MESSAGE_SINT, msr);

	rdmsrl(HV_X64_MSR_SIMP, msr);
	msr &= ~HV_SYNIC_SIMP_ENABLE;
	wrmsrl(HV_X64_MSR_SIMP, msr);

	rdmsrl(HV_X64_MSR_SIEFP, msr);
	msr &= ~HV_SYNIC_SIEFP_ENABLE;
	wrmsrl(HV_X64_MSR_SIEFP, msr);

	/* Disable the global synic bit */
	rdmsrl(HV_X64_MSR_SCONTROL, msr);
	msr &= ~HV_SYNIC_CONTROL_ENABLE;
	wrmsrl(HV_X64_MSR_SCONTROL, msr);
}
