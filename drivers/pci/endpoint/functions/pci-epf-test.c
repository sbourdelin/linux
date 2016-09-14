/**
 * pci-epf-test.c - Test driver to test endpoint functionality
 *
 * Copyright (C) 2016 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 of
 * the License as published by the Free Software Foundation.
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
#include <linux/slab.h>
#include <linux/pci_ids.h>

#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci_regs.h>

#define COMMAND_RESET			BIT(0)
#define COMMAND_RAISE_IRQ		BIT(1)
#define COMMAND_COPY			BIT(2)

#define STATUS_INITIALIZED		BIT(0)
#define STATUS_COPY_PROGRESS		BIT(1)
#define STATUS_COPY_DONE		BIT(2)
#define STATUS_IRQ_RAISED		BIT(3)
#define STATUS_SOURCE_ADDR_INVALID	BIT(4)
#define STATUS_DEST_ADDR_INVALID	BIT(5)

#define TIMER_RESOLUTION		5

struct pci_epf_test {
	struct timer_list	timer;
	void			*reg[6];
	struct pci_epf		*epf;
};

struct pci_epf_test_reg {
	u32	command;
	u32	status;
	u64	src_addr;
	u64	dst_addr;
};

struct pci_epf_header test_header = {
	.vendorid	= PCI_ANY_ID,
	.deviceid	= PCI_ANY_ID,
	.baseclass_code = PCI_CLASS_OTHERS,
	.interrupt_pin	= PCI_INTERRUPT_INTA,
};

static int bar_size[] = { 512, 1024, 16384, 131072, 1048576};

static void pci_epf_test_cmd_handler(unsigned long data)
{
	struct pci_epf_test *epf_test = (struct pci_epf_test *)data;
	struct pci_epf *epf = epf_test->epf;
	struct pci_epc *epc = epf->epc;
	struct pci_epf_test_reg *reg = epf_test->reg[0];
	unsigned long timer;

	if (!reg->command)
		goto reset_handler;

	if (reg->command & COMMAND_RESET)
		reg->status = STATUS_INITIALIZED;

	if (reg->command & COMMAND_RAISE_IRQ) {
		reg->status |= STATUS_IRQ_RAISED;
		pci_epc_raise_irq(epc, PCI_EPC_IRQ_LEGACY);
	}

	reg->command = 0;

reset_handler:
	timer = jiffies + msecs_to_jiffies(TIMER_RESOLUTION);
	mod_timer(&epf_test->timer, timer);
}

static void pci_epf_test_linkup(struct pci_epf *epf)
{
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);
	unsigned long timer;

	setup_timer(&epf_test->timer, pci_epf_test_cmd_handler,
		    (unsigned long)epf_test);
	timer = jiffies + msecs_to_jiffies(TIMER_RESOLUTION);
	mod_timer(&epf_test->timer, timer);
}

static void pci_epf_test_unbind(struct pci_epf *epf)
{
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	int bar;

	del_timer(&epf_test->timer);
	pci_epc_stop(epc);
	for (bar = BAR_0; bar <= BAR_5; bar++) {
		if (epf_test->reg[bar]) {
			pci_epf_free_space(epf, epf_test->reg[bar], bar);
			pci_epc_clear_bar(epc, bar);
		}
	}

	epf->pci_epc_name = NULL;
}

static int pci_epf_test_set_bar(struct pci_epf *epf)
{
	int flags;
	int bar;
	int ret;
	struct pci_epf_bar *epf_bar;
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);

	flags = PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_32;
	if (sizeof(dma_addr_t) == 0x8)
		flags |= PCI_BASE_ADDRESS_MEM_TYPE_64;

	for (bar = BAR_0; bar <= BAR_5; bar++) {
		epf_bar = &epf->bar[bar];
		ret = pci_epc_set_bar(epc, bar, epf_bar->phys_addr,
				      epf_bar->size, flags);
		if (ret) {
			pci_epf_free_space(epf, epf_test->reg[bar], bar);
			dev_err(dev, "failed to set BAR%d\n", bar);
			if (bar == BAR_0)
				return ret;
		}
	}

	return 0;
}

static int pci_epf_test_alloc_space(struct pci_epf *epf)
{
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);
	struct device *dev = &epf->dev;
	void *base;
	int bar;

	base = pci_epf_alloc_space(epf, sizeof(struct pci_epf_test_reg),
				   BAR_0);
	if (!base) {
		dev_err(dev, "failed to allocated register space\n");
		return -ENOMEM;
	}
	epf_test->reg[0] = base;

	for (bar = BAR_1; bar <= BAR_5; bar++) {
		base = pci_epf_alloc_space(epf, bar_size[bar - 1], bar);
		if (!base)
			dev_err(dev, "failed to allocate space for BAR%d\n",
				bar);
		epf_test->reg[bar] = base;
	}

	return 0;
}

static int pci_epf_test_bind(struct pci_epf *epf)
{
	int ret;
	struct pci_epf_header *header = epf->header;
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;

	ret = pci_epc_write_header(epc, header);
	if (ret) {
		dev_err(dev, "configuration header write failed\n");
		return ret;
	}

	ret = pci_epf_test_alloc_space(epf);
	if (ret)
		return ret;

	ret = pci_epf_test_set_bar(epf);
	if (ret)
		return ret;

	ret = pci_epc_start(epc);
	if (ret) {
		dev_err(dev, "failed to start endpoint controller\n");
		return ret;
	}

	return 0;
}

static int pci_epf_test_probe(struct pci_epf *epf)
{
	struct pci_epf_test *epf_test;
	struct device *dev = &epf->dev;

	epf_test = devm_kzalloc(dev, sizeof(*epf_test), GFP_KERNEL);
	if (!epf)
		return -ENOMEM;

	epf->header = &test_header;
	epf_test->epf = epf;

	epf_set_drvdata(epf, epf_test);
	return 0;
}

static int pci_epf_test_remove(struct pci_epf *epf)
{
	struct pci_epf_test *epf_test = epf_get_drvdata(epf);

	pci_epc_unbind_epf(epf_test->epf);
	kfree(epf_test);
	return 0;
}

struct pci_epf_ops ops = {
	.unbind	= pci_epf_test_unbind,
	.bind	= pci_epf_test_bind,
	.linkup = pci_epf_test_linkup,
};

static const struct pci_epf_device_id pci_epf_test_ids[] = {
	{
		.name = "pci_epf_test",
	},
	{},
};

static struct pci_epf_driver test_driver = {
	.driver.name	= "pci_epf_test",
	.probe		= pci_epf_test_probe,
	.remove		= pci_epf_test_remove,
	.id_table	= pci_epf_test_ids,
	.ops		= &ops,
	.owner		= THIS_MODULE,
};

static int __init pci_epf_test_init(void)
{
	int ret;

	ret = pci_epf_register_driver(&test_driver);
	if (ret) {
		pr_err("failed to register pci epf test driver --> %d\n", ret);
		return ret;
	}

	return 0;
}
module_init(pci_epf_test_init);

static void __exit pci_epf_test_exit(void)
{
	pci_epf_unregister_driver(&test_driver);
}
module_exit(pci_epf_test_exit);

MODULE_DESCRIPTION("PCI EPF TEST DRIVER");
MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
MODULE_LICENSE("GPL v2");
