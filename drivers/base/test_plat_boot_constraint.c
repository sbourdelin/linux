#include <linux/boot_constraint.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

static int test_constraints_probe(struct platform_device *platform_dev)
{
	struct device_node *np;
	struct boot_constraint_supply_info info = {
		.enable = true,
		.name = "vmmc",
		.u_volt_min = 1800000,
		.u_volt_max = 3000000,
	};
	struct platform_device *pdev;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "hisilicon,hi6220-dw-mshc");
	if (!np)
		return -ENODEV;

	pdev = of_find_device_by_node(np);
	of_node_put(np);

	if (!pdev) {
		pr_err("%s: device not found\n", __func__);
		return -ENODEV;
	}

	ret = boot_constraint_add(&pdev->dev, BOOT_CONSTRAINT_SUPPLY, &info);
	if (ret)
		return ret;

	return ret;
}

static struct platform_driver test_constraints_driver = {
	.driver = {
		.name	= "test-constraints",
	},
	.probe	= test_constraints_probe,
};

static int __init test_constraints_init(void)
{
	platform_device_register_data(NULL, "test-constraints", -1, NULL, 0);

	return platform_driver_register(&test_constraints_driver);
}
subsys_initcall(test_constraints_init);
