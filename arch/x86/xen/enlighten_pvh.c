#include <linux/acpi.h>

#include <xen/hvc-console.h>

#include <asm/io_apic.h>
#include <asm/hypervisor.h>
#include <asm/e820/api.h>

#include <asm/xen/interface.h>
#include <asm/xen/hypercall.h>

#include <xen/interface/memory.h>
#include <xen/interface/hvm/start_info.h>

/*
 * PVH variables.
 *
 * xen_pvh and pvh_bootparams need to live in data segment since they
 * are used after startup_{32|64}, which clear .bss, are invoked.
 */
bool xen_pvh __attribute__((section(".data"))) = 0;
struct boot_params pvh_bootparams __attribute__((section(".data")));

struct hvm_start_info pvh_start_info;
unsigned int pvh_start_info_sz = sizeof(pvh_start_info);

static void xen_pvh_arch_setup(void)
{
	/* Make sure we don't fall back to (default) ACPI_IRQ_MODEL_PIC. */
	if (nr_ioapics == 0)
		acpi_irq_model = ACPI_IRQ_MODEL_PLATFORM;
}

static void __init init_pvh_bootparams(bool xen_guest)
{
	struct xen_memory_map memmap;
	int rc;

	memset(&pvh_bootparams, 0, sizeof(pvh_bootparams));

	if ((pvh_start_info.version > 0) && (pvh_start_info.memmap_entries)) {
		struct hvm_memmap_table_entry *ep;
		int i;

		ep = __va(pvh_start_info.memmap_paddr);
		pvh_bootparams.e820_entries = pvh_start_info.memmap_entries;

		for (i = 0; i < pvh_bootparams.e820_entries ; i++, ep++) {
			pvh_bootparams.e820_table[i].addr = ep->addr;
			pvh_bootparams.e820_table[i].size = ep->size;
			pvh_bootparams.e820_table[i].type = ep->type;
		}
	} else if (xen_guest) {
		memmap.nr_entries = ARRAY_SIZE(pvh_bootparams.e820_table);
		set_xen_guest_handle(memmap.buffer, pvh_bootparams.e820_table);
		rc = HYPERVISOR_memory_op(XENMEM_memory_map, &memmap);
		if (rc) {
			xen_raw_printk("XENMEM_memory_map failed (%d)\n", rc);
			BUG();
		}
		pvh_bootparams.e820_entries = memmap.nr_entries;
	} else {
		xen_raw_printk("Error: Could not find memory map\n");
		BUG();
	}

	if (pvh_bootparams.e820_entries < E820_MAX_ENTRIES_ZEROPAGE - 1) {
		pvh_bootparams.e820_table[pvh_bootparams.e820_entries].addr =
			ISA_START_ADDRESS;
		pvh_bootparams.e820_table[pvh_bootparams.e820_entries].size =
			ISA_END_ADDRESS - ISA_START_ADDRESS;
		pvh_bootparams.e820_table[pvh_bootparams.e820_entries].type =
			E820_TYPE_RESERVED;
		pvh_bootparams.e820_entries++;
	} else
		xen_raw_printk("Warning: Can fit ISA range into e820\n");

	pvh_bootparams.hdr.cmd_line_ptr =
		pvh_start_info.cmdline_paddr;

	/* The first module is always ramdisk. */
	if (pvh_start_info.nr_modules) {
		struct hvm_modlist_entry *modaddr =
			__va(pvh_start_info.modlist_paddr);
		pvh_bootparams.hdr.ramdisk_image = modaddr->paddr;
		pvh_bootparams.hdr.ramdisk_size = modaddr->size;
	}

	/*
	 * See Documentation/x86/boot.txt.
	 *
	 * Version 2.12 supports Xen entry point but we will use default x86/PC
	 * environment (i.e. hardware_subarch 0).
	 */
	pvh_bootparams.hdr.version = 0x212;
	pvh_bootparams.hdr.type_of_loader = ((xen_guest ? 0x9 : 0xb) << 4) | 0;
}

/*
 * This routine (and those that it might call) should not use
 * anything that lives in .bss since that segment will be cleared later.
 */
void __init xen_prepare_pvh(void)
{

	u32 msr = xen_cpuid_base();
	u64 pfn;
	bool xen_guest = !!msr;

	if (pvh_start_info.magic != XEN_HVM_START_MAGIC_VALUE) {
		xen_raw_printk("Error: Unexpected magic value (0x%08x)\n",
				pvh_start_info.magic);
		BUG();
	}

	if (xen_guest) {
		xen_pvh = 1;

		msr = cpuid_ebx(msr + 2);
		pfn = __pa(hypercall_page);
		wrmsr_safe(msr, (u32)pfn, (u32)(pfn >> 32));

		x86_init.oem.arch_setup = xen_pvh_arch_setup;
	}

	init_pvh_bootparams(xen_guest);
}
