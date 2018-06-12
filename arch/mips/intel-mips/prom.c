// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2014 Lei Chuanhua <Chuanhua.lei@lantiq.com>
 * Copyright (C) 2016 Intel Corporation.
 */
#include <linux/init.h>
#include <linux/export.h>
#include <linux/of_platform.h>
#include <linux/of_fdt.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <asm/mips-cps.h>
#include <asm/smp-ops.h>
#include <asm/dma-coherence.h>
#include <asm/prom.h>

#define IOPORT_RESOURCE_START   0x10000000
#define IOPORT_RESOURCE_END     0xffffffff
#define IOMEM_RESOURCE_START    0x10000000
#define IOMEM_RESOURCE_END      0xffffffff

const char *get_system_type(void)
{
	return "Intel MIPS interAptiv SoC";
}

void prom_free_prom_memory(void)
{
}

static void __init prom_init_cmdline(void)
{
	int i;
	int argc;
	char **argv;

	/*
	 * If u-boot pass parameters, it is ok, however, if without u-boot
	 * JTAG or other tool has to reset all register value before it goes
	 * emulation most likely belongs to this category
	 */
	if (fw_arg0 == 0 || fw_arg1 == 0)
		return;

	argc = fw_arg0;
	argv = (char **)KSEG1ADDR(fw_arg1);

	arcs_cmdline[0] = '\0';

	for (i = 0; i < argc; i++) {
		char *p = (char *)KSEG1ADDR(argv[i]);

		if (argv[i] && *p) {
			strlcat(arcs_cmdline, p, sizeof(arcs_cmdline));
			strlcat(arcs_cmdline, " ", sizeof(arcs_cmdline));
		}
	}
}

static int __init plat_enable_iocoherency(void)
{
	int supported = 0;

	if (mips_cps_numiocu(0) != 0) {
		/* Nothing special needs to be done to enable coherency */
		pr_info("Coherence Manager IOCU detected\n");
		/* Second IOCU for MPE or other master access register */
		write_gcr_reg0_base(0xa0000000);
		write_gcr_reg0_mask(0xf8000000 | CM_GCR_REGn_MASK_CMTGT_IOCU1);
		supported = 1;
	}

	/* hw_coherentio = supported; */

	return supported;
}

static void __init plat_setup_iocoherency(void)
{
#ifdef CONFIG_DMA_NONCOHERENT
	/*
	 * Kernel has been configured with software coherency
	 * but we might choose to turn it off and use hardware
	 * coherency instead.
	 */
	if (plat_enable_iocoherency()) {
		if (coherentio == IO_COHERENCE_DISABLED)
			pr_info("Hardware DMA cache coherency disabled\n");
		else
			pr_info("Hardware DMA cache coherency enabled\n");
	} else {
		if (coherentio == IO_COHERENCE_ENABLED)
			pr_info("Hardware DMA cache coherency unsupported, but enabled from command line!\n");
		else
			pr_info("Software DMA cache coherency enabled\n");
	}
#else
	if (!plat_enable_iocoherency())
		panic("Hardware DMA cache coherency not supported!");
#endif
}

static void free_init_pages_eva_intel(void *begin, void *end)
{
	free_init_pages("unused kernel", __pa_symbol((unsigned long *)begin),
			__pa_symbol((unsigned long *)end));
}

static void plat_early_init_devtree(void)
{
	void *dtb;

	/*
	 * Load the builtin devicetree. This causes the chosen node to be
	 * parsed resulting in our memory appearing
	 */
	if (fw_passed_dtb) /* used by CONFIG_MIPS_APPENDED_RAW_DTB as well */
		dtb = (void *)fw_passed_dtb;
	else if (__dtb_start != __dtb_end)
		dtb = (void *)__dtb_start;

	if (dtb)
		__dt_setup_arch(dtb);
}

void __init plat_mem_setup(void)
{
	ioport_resource.start = IOPORT_RESOURCE_START;
	ioport_resource.end = ~0UL; /* No limit */
	iomem_resource.start = IOMEM_RESOURCE_START;
	iomem_resource.end = ~0UL; /* No limit */

	set_io_port_base((unsigned long)KSEG1);

	strlcpy(arcs_cmdline, boot_command_line, COMMAND_LINE_SIZE);

	plat_early_init_devtree();
	plat_setup_iocoherency();

	if (IS_ENABLED(CONFIG_EVA))
		free_init_pages_eva = free_init_pages_eva_intel;
	else
		free_init_pages_eva = 0;
}

void __init device_tree_init(void)
{
	if (!initial_boot_params)
		return;

	unflatten_and_copy_device_tree();
}

#define CPC_BASE_ADDR		0x12310000

phys_addr_t mips_cpc_default_phys_base(void)
{
	return CPC_BASE_ADDR;
}

void __init prom_init(void)
{
	prom_init_cmdline();

	mips_cpc_probe();

	if (!register_cps_smp_ops())
		return;

	if (!register_cmp_smp_ops())
		return;

	if (!register_vsmp_smp_ops())
		return;
}

static int __init plat_publish_devices(void)
{
	if (!of_have_populated_dt())
		return 0;
	return of_platform_populate(NULL, of_default_bus_match_table, NULL,
				    NULL);
}
arch_initcall(plat_publish_devices);
