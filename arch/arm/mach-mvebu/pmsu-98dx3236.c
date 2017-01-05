/**
 * CPU resume support for 98DX3236 internal CPU (a.k.a. MSYS).
 */

#define pr_fmt(fmt) "mv98dx3236-resume: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include "common.h"

static void __iomem *mv98dx3236_resume_base;
#define MV98DX3236_CPU_RESUME_CTRL_OFFSET	0x08
#define MV98DX3236_CPU_RESUME_ADDR_OFFSET	0x04

static const struct of_device_id of_mv98dx3236_resume_table[] = {
	{.compatible = "marvell,98dx3336-resume-ctrl",},
	{ /* end of list */ },
};

void mv98dx3236_resume_set_cpu_boot_addr(int hw_cpu, void *boot_addr)
{
	WARN_ON(hw_cpu != 1);

	writel(0, mv98dx3236_resume_base + MV98DX3236_CPU_RESUME_CTRL_OFFSET);
	writel(virt_to_phys(boot_addr), mv98dx3236_resume_base +
	       MV98DX3236_CPU_RESUME_ADDR_OFFSET);
}

static int __init mv98dx3236_resume_init(void)
{
	struct device_node *np;
	struct resource res;
	int ret = 0;

	np = of_find_matching_node(NULL, of_mv98dx3236_resume_table);
	if (!np)
		return 0;

	pr_info("Initializing 98DX3236 Resume\n");

	if (of_address_to_resource(np, 0, &res)) {
		pr_err("unable to get resource\n");
		ret = -ENOENT;
		goto out;
	}

	if (!request_mem_region(res.start, resource_size(&res),
				np->full_name)) {
		pr_err("unable to request region\n");
		ret = -EBUSY;
		goto out;
	}

	mv98dx3236_resume_base = ioremap(res.start, resource_size(&res));
	if (!mv98dx3236_resume_base) {
		pr_err("unable to map registers\n");
		release_mem_region(res.start, resource_size(&res));
		ret = -ENOMEM;
		goto out;
	}

out:
	of_node_put(np);
	return ret;
}

early_initcall(mv98dx3236_resume_init);
