/*
 * AMD Seattle EDAC
 *
 * Copyright (c) 2015, Advanced Micro Devices
 * Author: Brijesh Singh <brijeshkumar.singh@amd.com>
 *
 * The driver polls CPUMERRSR_EL1 and L2MERRSR_EL1 registers to logs the 
 * non-fatal errors. Whereas the single bit and double bit ECC erros are 
 * handled by firmware.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "edac_core.h"

#define EDAC_MOD_STR             "seattle_edac"

#define CPUMERRSR_EL1_INDEX(x)   ((x) & 0x1ffff)
#define CPUMERRSR_EL1_BANK(x)    (((x) >> 18) & 0x1f)
#define CPUMERRSR_EL1_RAMID(x)   (((x) >> 24) & 0x7f)
#define CPUMERRSR_EL1_VALID(x)   ((x) & (1 << 31))
#define CPUMERRSR_EL1_REPEAT(x)  (((x) >> 32) & 0x7f)
#define CPUMERRSR_EL1_OTHER(x)   (((x) >> 40) & 0xff)
#define CPUMERRSR_EL1_FATAL(x)   ((x) & (1UL << 63))

#define L2MERRSR_EL1_INDEX(x)    ((x) & 0x1ffff)
#define L2MERRSR_EL1_CPUID(x)    (((x) >> 18) & 0xf)
#define L2MERRSR_EL1_RAMID(x)    (((x) >> 24) & 0x7f)
#define L2MERRSR_EL1_VALID(x)    ((x) & (1 << 31))
#define L2MERRSR_EL1_REPEAT(x)   (((x) >> 32) & 0xff)
#define L2MERRSR_EL1_OTHER(x)    (((x) >> 40) & 0xff)
#define L2MERRSR_EL1_FATAL(x)    ((x) & (1UL << 63))

struct seattle_edac {
	struct edac_device_ctl_info *edac_ctl;
};

static inline u64 read_cpumerrsr_el1(void)
{
	u64 val;

	asm volatile("mrs %0, s3_1_c15_c2_2" : "=r" (val));
	return val;
}

static inline void write_cpumerrsr_el1(u64 val)
{
	asm volatile("msr s3_1_c15_c2_2, %0" :: "r" (val));
}

static inline u64 read_l2merrsr_el1(void)
{
	u64 val;

	asm volatile("mrs %0, s3_1_c15_c2_3" : "=r" (val));
	return val;
}

static inline void write_l2merrsr_el1(u64 val)
{
	asm volatile("msr s3_1_c15_c2_3, %0" :: "r" (val));
}

static void check_l2merrsr_el1_error(struct edac_device_ctl_info *edac_ctl)
{
	int fatal;
	int cpuid;
	u64 val = read_l2merrsr_el1();

	if (!L2MERRSR_EL1_VALID(val))
		return;

	fatal = L2MERRSR_EL1_FATAL(val);
	cpuid = L2MERRSR_EL1_CPUID(val);
	edac_printk(KERN_CRIT, EDAC_MOD_STR,
		    "CPU%d detected %s error on L2 (L2MERRSR=%#llx)!\n",
		    smp_processor_id(), fatal ? "fatal" : "non-fatal", val);

	switch (L2MERRSR_EL1_RAMID(val)) {
	case 0x10:
		edac_printk(KERN_CRIT, EDAC_MOD_STR,
			    "L2 Tag RAM cpu %d way %d\n", cpuid / 2, cpuid % 2);
		break;
	case 0x11:
		edac_printk(KERN_CRIT, EDAC_MOD_STR,
			    "L2 Data RAM cpu %d way %d\n", cpuid / 2, cpuid % 2);
		break;
	case 0x12:
		edac_printk(KERN_CRIT, EDAC_MOD_STR,
			    "L2 Snoop tag RAM cpu %d way %d\n",
			    cpuid / 2, cpuid % 2);
		break;
	case 0x14:
		edac_printk(KERN_CRIT, EDAC_MOD_STR,
			    "L2 Dirty RAM cpu %d way %d\n",
			    cpuid / 2, cpuid % 2);
		break;
	case 0x18:
		edac_printk(KERN_CRIT, EDAC_MOD_STR,
			    "L2 inclusion RAM cpu %d way %d\n",
			    cpuid / 2, cpuid % 2);
		break;
	default:
		edac_printk(KERN_CRIT, EDAC_MOD_STR,
			    "unknown RAMID cpuid %d\n", cpuid);
		break;
	}

	edac_printk(KERN_CRIT, EDAC_MOD_STR, "Repeated error count: %d\n",
		    (int)L2MERRSR_EL1_REPEAT(val));
	edac_printk(KERN_CRIT, EDAC_MOD_STR, "Other error count: %d\n",
		    (int)L2MERRSR_EL1_OTHER(val));
	if (fatal)
		edac_device_handle_ue(edac_ctl, smp_processor_id(), 1,
				      edac_ctl->name);
	else
		edac_device_handle_ce(edac_ctl, smp_processor_id(), 1,
				      edac_ctl->name);
	write_l2merrsr_el1(0);
}

static void check_cpumerrsr_el1_error(struct edac_device_ctl_info *edac_ctl)
{
	int fatal;
	int bank;
	u64 val = read_cpumerrsr_el1();

	if (!CPUMERRSR_EL1_VALID(val))
		return;

	bank = CPUMERRSR_EL1_BANK(val);
	fatal = CPUMERRSR_EL1_FATAL(val);
	edac_printk(KERN_CRIT, EDAC_MOD_STR,
		    "CPU%d detected %s error on L1 (CPUMERRSR=%#llx)!\n",
		    smp_processor_id(), fatal ? "fatal" : "non-fatal", val);

	switch (CPUMERRSR_EL1_RAMID(val)) {
	case 0x0:
		edac_printk(KERN_CRIT, EDAC_MOD_STR,
			    "L1-I Tag RAM bank %d\n", bank);
		break;
	case 0x1:
		edac_printk(KERN_CRIT, EDAC_MOD_STR,
			    "L1-I Data RAM bank %d\n", bank);
		break;
	case 0x8:
		edac_printk(KERN_CRIT, EDAC_MOD_STR,
			    "L1-D Tag RAM bank %d\n", bank);
		break;
	case 0x9:
		edac_printk(KERN_CRIT, EDAC_MOD_STR,
			    "L1-D Data RAM bank %d\n", bank);
		break;
	case 0x18:
		edac_printk(KERN_CRIT, EDAC_MOD_STR,
			    "L2 TLB RAM bank %d\n", bank);
		break;
	default:
		edac_printk(KERN_CRIT, EDAC_MOD_STR,
			    "unknown ramid %d bank %d\n",
			    (int)CPUMERRSR_EL1_RAMID(val), bank);
		break;
	}

	edac_printk(KERN_CRIT, EDAC_MOD_STR, "Repeated error count: %d\n",
		    (int)CPUMERRSR_EL1_REPEAT(val));
	edac_printk(KERN_CRIT, EDAC_MOD_STR, "Other error count: %d\n",
		    (int)CPUMERRSR_EL1_OTHER(val));
	if (fatal)
		edac_device_handle_ue(edac_ctl, smp_processor_id(), 1,
				      edac_ctl->name);
	else
		edac_device_handle_ce(edac_ctl, smp_processor_id(), 1,
				      edac_ctl->name);
	write_cpumerrsr_el1(0);
}

static void cpu_check_errors(void *args)
{
	struct edac_device_ctl_info *edev_ctl = args;

	check_cpumerrsr_el1_error(edev_ctl);
	check_l2merrsr_el1_error(edev_ctl);
}

static void edac_check_errors(struct edac_device_ctl_info *edev_ctl)
{
	int cpu;

	/* read L1 and L2 memory error syndrome register on possible CPU's */
	for_each_possible_cpu(cpu)
		smp_call_function_single(cpu, cpu_check_errors, edev_ctl, 0);
}

static int seattle_edac_probe(struct platform_device *pdev)
{
	int rc;
	u32 poll_msec;
	struct seattle_edac *drv;
	struct device *dev = &pdev->dev;

	rc = of_property_read_u32(pdev->dev.of_node, "poll-delay-msec",
				  &poll_msec);
	if (rc < 0) {
		edac_printk(KERN_ERR, EDAC_MOD_STR,
			    "failed to get poll interval\n");
		return rc;
	}

	drv = devm_kzalloc(dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->edac_ctl = edac_device_alloc_ctl_info(0, "cpu",
						   num_possible_cpus(), "L", 2,
						   1, NULL, 0,
						   edac_device_alloc_index());

	drv->edac_ctl->poll_msec = poll_msec;
	drv->edac_ctl->edac_check = edac_check_errors;
	drv->edac_ctl->dev = dev;
	drv->edac_ctl->mod_name = dev_name(dev);
	drv->edac_ctl->dev_name = dev_name(dev);
	drv->edac_ctl->ctl_name = "cpu_err";
	drv->edac_ctl->panic_on_ue = 1;
	platform_set_drvdata(pdev, drv);

	rc = edac_device_add_device(drv->edac_ctl);
	if (rc)
		goto edac_alloc_failed;

	return 0;

edac_alloc_failed:
	edac_device_free_ctl_info(drv->edac_ctl);
	return rc;
}

static int seattle_edac_remove(struct platform_device *pdev)
{
	struct seattle_edac *drv = dev_get_drvdata(&pdev->dev);
	struct edac_device_ctl_info *edac_ctl = drv->edac_ctl;

	edac_device_del_device(edac_ctl->dev);
	edac_device_free_ctl_info(edac_ctl);

	return 0;
}

static const struct of_device_id seattle_edac_of_match[] = {
	{ .compatible = "amd,arm-seattle-edac" },
	{},
};
MODULE_DEVICE_TABLE(of, seattle_edac_of_match);

static struct platform_driver seattle_edac_driver = {
	.probe = seattle_edac_probe,
	.remove = seattle_edac_remove,
	.driver = {
		.name = "seattle-edac",
		.of_match_table = seattle_edac_of_match,
	},
};

static int __init seattle_edac_init(void)
{
	int rc;

	/* we support poll method */
	edac_op_state = EDAC_OPSTATE_POLL;

	rc = platform_driver_register(&seattle_edac_driver);
	if (rc) {
		edac_printk(KERN_ERR, EDAC_MOD_STR,
			    "EDAC fails to register\n");
		return rc;
	}

	return 0;
}
module_init(seattle_edac_init);

static void __exit seattle_edac_exit(void)
{
	platform_driver_unregister(&seattle_edac_driver);
}
module_exit(seattle_edac_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Brijesh Singh <brijeshkumar.singh@amd.com>");
MODULE_DESCRIPTION("AMD Seattle EDAC driver");
