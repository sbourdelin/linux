// SPDX-License-Identifier: GPL-2.0+
/* The BGX nexus consists of a group of up to four Ethernet MACs (the
 * LMACs).  This driver manages the LMACs and creates a child device
 * for each of the configured LMACs.
 *
 * Copyright (c) 2017 Cavium, Inc.
 */
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/list.h>

#include "octeon3.h"

static atomic_t request_mgmt_once;
static atomic_t load_driver_once;
static atomic_t pki_id;

#define MAX_MIX_PER_NODE	2

#define MAX_MIX			(MAX_NODES * MAX_MIX_PER_NODE)

/**
 * struct mix_port_lmac - Describes a lmac that connects to a mix
 *			  port. The lmac must be on the same node as
 *			  the mix.
 * @node: Node of the lmac.
 * @bgx:  Bgx of the lmac.
 * @lmac: Lmac index.
 */
struct mix_port_lmac {
	int	node;
	int	bgx;
	int	lmac;
};

/* mix_ports_lmacs contains all the lmacs connected to mix ports */
static struct mix_port_lmac mix_port_lmacs[MAX_MIX];

/* pki_ports keeps track of the lmacs connected to the pki */
static bool pki_ports[MAX_NODES][MAX_BGX_PER_NODE][MAX_LMAC_PER_BGX];

/* Created platform devices get added to this list */
static struct list_head pdev_list;
static struct mutex pdev_list_lock;

/* Created platform device use this structure to add themselves to the list */
struct pdev_list_item {
	struct list_head	list;
	struct platform_device	*pdev;
};

/**
 * is_lmac_to_mix() - Search the list of lmacs connected to mix'es for a match.
 * @node: Numa node of lmac to search for.
 * @bgx:  Bgx of lmac to search for.
 * @lmac: Lmac index to search for.
 *
 * Return: true if the lmac is connected to a mix.
 *         false if the lmac is not connected to a mix.
 */
static bool is_lmac_to_mix(int node, int bgx, int lmac)
{
	int i;

	for (i = 0; i < MAX_MIX; i++) {
		if (mix_port_lmacs[i].node == node &&
		    mix_port_lmacs[i].bgx == bgx &&
		    mix_port_lmacs[i].lmac == lmac)
			return true;
	}

	return false;
}

/**
 * is_lmac_to_pki() - Search the list of lmacs connected to the pki for a match.
 * @node: Numa node of lmac to search for.
 * @bgx:  Bgx of lmac to search for.
 * @lmac: Lmac index to search for.
 *
 * Return: true if the lmac is connected to the pki.
 *         false if the lmac is not connected to the pki.
 */
static bool is_lmac_to_pki(int node, int bgx, int lmac)
{
	return pki_ports[node][bgx][lmac];
}

/**
 * is_lmac_to_xcv() - Check if this lmac is connected to the xcv block (rgmii).
 * @of_node: Device node to check.
 *
 * Return: true if the lmac is connected to the xcv port.
 *         false if the lmac is not connected to the xcv port.
 */
static bool is_lmac_to_xcv(struct device_node *of_node)
{
	return of_device_is_compatible(of_node, "cavium,octeon-7360-xcv");
}

static int bgx_probe(struct platform_device *pdev)
{
	struct mac_platform_data platform_data;
	struct platform_device *new_dev;
	struct platform_device *pki_dev;
	struct device_node *child;
	const __be32 *reg;
	int interface;
	int numa_node;
	char id[64];
	int r = 0;
	u64 addr;
	u32 port;
	u64 data;
	int i;

	reg = of_get_property(pdev->dev.of_node, "reg", NULL);
	addr = of_translate_address(pdev->dev.of_node, reg);
	interface = bgx_addr_to_interface(addr);
	numa_node = bgx_node_to_numa_node(pdev->dev.of_node);

	/* Assign 8 CAM entries per LMAC */
	for (i = 0; i < 32; i++) {
		data = i >> 3;
		oct_csr_write(data, BGX_CMR_RX_ADRX_CAM(numa_node, interface, i));
	}

	for_each_available_child_of_node(pdev->dev.of_node, child) {
		struct pdev_list_item *pdev_item;
		bool is_mix = false;
		bool is_pki = false;
		bool is_xcv = false;

		if (!of_device_is_compatible(child, "cavium,octeon-7890-bgx-port") &&
		    !of_device_is_compatible(child, "cavium,octeon-7360-xcv"))
			continue;
		r = of_property_read_u32(child, "reg", &port);
		if (r)
			return -ENODEV;

		is_mix = is_lmac_to_mix(numa_node, interface, port);
		is_pki = is_lmac_to_pki(numa_node, interface, port);
		is_xcv = is_lmac_to_xcv(child);

		/* Check if this port should be configured */
		if (!is_mix && !is_pki)
			continue;

		/* Connect to PKI/PKO */
		data = oct_csr_read(BGX_CMR_CONFIG(numa_node, interface, port));
		if (is_mix)
			data |= BIT(11);
		else
			data &= ~BIT(11);
		oct_csr_write(data, BGX_CMR_CONFIG(numa_node, interface, port));

		/* Unreset the mix bgx interface or it will interfare with the
		 * other ports.
		 */
		if (is_mix) {
			data = oct_csr_read(BGX_CMR_GLOBAL_CONFIG(numa_node, interface));
			if (!port)
				data &= ~BIT(3);
			else if (port == 1)
				data &= ~BIT(4);
			oct_csr_write(data, BGX_CMR_GLOBAL_CONFIG(numa_node, interface));
		}

		snprintf(id, sizeof(id), "%llx.%u.ethernet-mac",
			 (unsigned long long)addr, port);
		new_dev = of_platform_device_create(child, id, &pdev->dev);
		if (!new_dev) {
			dev_err(&pdev->dev, "Error creating %s\n", id);
			continue;
		}
		platform_data.mac_type = BGX_MAC;
		platform_data.numa_node = numa_node;
		platform_data.interface = interface;
		platform_data.port = port;
		if (is_xcv)
			platform_data.src_type = XCV;
		else
			platform_data.src_type = QLM;

		/* Add device to the list of created devices so we can
		 * remove it on exit.
		 */
		pdev_item = kmalloc(sizeof(*pdev_item), GFP_KERNEL);
		pdev_item->pdev = new_dev;
		mutex_lock(&pdev_list_lock);
		list_add(&pdev_item->list, &pdev_list);
		mutex_unlock(&pdev_list_lock);

		i = atomic_inc_return(&pki_id);
		pki_dev = platform_device_register_data(&new_dev->dev,
							is_mix ? "octeon_mgmt" : "ethernet-mac-pki",
							i, &platform_data,
							sizeof(platform_data));
		dev_info(&pdev->dev, "Created %s %u\n",
			 is_mix ? "MIX" : "PKI", pki_dev->id);

		/* Add device to the list of created devices so we can
		 * remove it on exit.
		 */
		pdev_item = kmalloc(sizeof(*pdev_item), GFP_KERNEL);
		pdev_item->pdev = pki_dev;
		mutex_lock(&pdev_list_lock);
		list_add(&pdev_item->list, &pdev_list);
		mutex_unlock(&pdev_list_lock);

#ifdef CONFIG_NUMA
		new_dev->dev.numa_node = pdev->dev.numa_node;
		pki_dev->dev.numa_node = pdev->dev.numa_node;
#endif
		/* One time request driver module */
		if (is_mix) {
			if (atomic_cmpxchg(&request_mgmt_once, 0, 1) == 0)
				request_module_nowait("octeon_mgmt");
		}
		if (is_pki) {
			if (atomic_cmpxchg(&load_driver_once, 0, 1) == 0)
				request_module_nowait("octeon3-ethernet");
		}
	}

	dev_info(&pdev->dev, "Probed\n");
	return 0;
}

/**
 * bgx_mix_init_from_fdt() - Initialize the list of lmacs that connect to mix
 *			     ports from information in the device tree.
 *
 * Return: 0 if successful.
 *         < 0 for error codes.
 */
static int bgx_mix_init_from_fdt(void)
{
	struct device_node *parent = NULL;
	struct device_node *node;
	int mix = 0;

	for_each_compatible_node(node, NULL, "cavium,octeon-7890-mix") {
		struct device_node *lmac_fdt_node;
		const __be32 *reg;
		u64 addr;
		u32 t;

		/* Get the fdt node of the lmac connected to this mix */
		lmac_fdt_node = of_parse_phandle(node, "cavium,mac-handle", 0);
		if (!lmac_fdt_node)
			goto err;

		/* Get the numa node and bgx of the lmac */
		parent = of_get_parent(lmac_fdt_node);
		if (!parent)
			goto err;
		reg = of_get_property(parent, "reg", NULL);
		if (!reg)
			goto err;
		addr = of_translate_address(parent, reg);
		of_node_put(parent);
		parent = NULL;

		mix_port_lmacs[mix].node = bgx_node_to_numa_node(parent);
		mix_port_lmacs[mix].bgx = bgx_addr_to_interface(addr);

		/* Get the lmac index */
		if (!of_property_read_u32(lmac_fdt_node, "reg", &t))
			goto err;

		mix_port_lmacs[mix].lmac = t;

		mix++;
		if (mix >= MAX_MIX)
			break;
	}

	return 0;
 err:
	pr_warn("Invalid device tree mix port information\n");
	for (mix = 0; mix < MAX_MIX; mix++) {
		mix_port_lmacs[mix].node = -1;
		mix_port_lmacs[mix].bgx = -1;
		mix_port_lmacs[mix].lmac = -1;
	}
	if (parent)
		of_node_put(parent);

	return -EINVAL;
}

/**
 * bgx_mix_port_lmacs_init() - Initialize the mix_port_lmacs variable with the
 *			       lmacs that connect to mix ports.
 *
 * Return: 0 if successful.
 *         < 0 for error codes.
 */
static int bgx_mix_port_lmacs_init(void)
{
	int mix;

	/* Start with no mix ports configured */
	for (mix = 0; mix < MAX_MIX; mix++) {
		mix_port_lmacs[mix].node = -1;
		mix_port_lmacs[mix].bgx = -1;
		mix_port_lmacs[mix].lmac = -1;
	}

	/* Configure the mix ports using information from the device tree if no
	 * parameter was passed. Otherwise, use the information in the module
	 * parameter.
	 */
	bgx_mix_init_from_fdt();

	return 0;
}

/**
 * bgx_pki_ports_init() - Initialize the pki_ports variable with the lmacs that
 *			  connect to the pki.
 *
 * Returns 0 if successful.
 * Returns < 0 for error codes.
 */
static int bgx_pki_ports_init(void)
{
	int i;
	int j;
	int k;

	for (i = 0; i < MAX_NODES; i++) {
		for (j = 0; j < MAX_BGX_PER_NODE; j++) {
			for (k = 0; k < MAX_LMAC_PER_BGX; k++)
				pki_ports[i][j][k] = true;
		}
	}

	return 0;
}

static int bgx_remove(struct platform_device *pdev)
{
	return 0;
}

static void bgx_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id bgx_match[] = {
	{
		.compatible = "cavium,octeon-7890-bgx",
	},
	{},
};
MODULE_DEVICE_TABLE(of, bgx_match);

static struct platform_driver bgx_driver = {
	.probe		= bgx_probe,
	.remove		= bgx_remove,
	.shutdown       = bgx_shutdown,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= KBUILD_MODNAME,
		.of_match_table = bgx_match,
	},
};

/* Allow bgx_port driver to force this driver to load */
void bgx_nexus_load(void)
{
}
EXPORT_SYMBOL(bgx_nexus_load);

static int __init bgx_driver_init(void)
{
	int r;

	INIT_LIST_HEAD(&pdev_list);
	mutex_init(&pdev_list_lock);

	bgx_mix_port_lmacs_init();
	bgx_pki_ports_init();

	r = platform_driver_register(&bgx_driver);

	return r;
}

static void __exit bgx_driver_exit(void)
{
	struct pdev_list_item *pdev_item;

	mutex_lock(&pdev_list_lock);
	while (!list_empty(&pdev_list)) {
		pdev_item = list_first_entry(&pdev_list,
					     struct pdev_list_item, list);
		list_del(&pdev_item->list);
		platform_device_unregister(pdev_item->pdev);
		kfree(pdev_item);
	}
	mutex_unlock(&pdev_list_lock);

	platform_driver_unregister(&bgx_driver);
}

module_init(bgx_driver_init);
module_exit(bgx_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cavium, Inc. <support@caviumnetworks.com>");
MODULE_DESCRIPTION("Cavium, Inc. BGX MAC Nexus driver.");
