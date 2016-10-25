/* pci-stub - simple stub driver to reserve a pci device
 *
 * Copyright (C) 2008 Red Hat, Inc.
 * Author:
 *	Chris Wright
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 *
 * Usage is simple, allocate a new id to the stub driver and bind the
 * device to it.  For example:
 *
 * # echo "8086 10f5" > /sys/bus/pci/drivers/pci-stub/new_id
 * # echo -n 0000:00:19.0 > /sys/bus/pci/drivers/e1000e/unbind
 * # echo -n 0000:00:19.0 > /sys/bus/pci/drivers/pci-stub/bind
 * # ls -l /sys/bus/pci/devices/0000:00:19.0/driver
 * .../0000:00:19.0/driver -> ../../../bus/pci/drivers/pci-stub
 */

#include <linux/module.h>
#include <linux/pci.h>

static char ids[1024] __initdata;

module_param_string(ids, ids, sizeof(ids), 0);
MODULE_PARM_DESC(ids, "Initial PCI IDs to add to the stub driver, format is "
		 "\"vendor:device[:subvendor[:subdevice[:class[:class_mask]]]]\""
		 " and multiple comma separated entries can be specified");

#define MAX_EXCEPT 16

static unsigned num_except;
static struct except {
	u16 domain;
	u16 devid;
} except[MAX_EXCEPT];

/*
 * Accommodate substrings like "0000:00:1c.4," MAX_EXCEPT times, with the comma
 * replaced with '\0' in the last instance
 */
static char except_str[13 * MAX_EXCEPT] __initdata;

module_param_string(except, except_str, sizeof except_str, 0);
MODULE_PARM_DESC(except, "Comma-separated list of PCI addresses to except "
		 "from the ID- and class-based binding. The address format is "
		 "Domain:Bus:Device.Function (all components are required and "
		 "written in hex), for example, 0000:00:1c.4. At most "
		 __stringify(MAX_EXCEPT) " exceptions are supported.");

static inline bool exception_matches(const struct except *ex,
				     const struct pci_dev *dev)
{
	return ex->domain == pci_domain_nr(dev->bus) &&
	       ex->devid == PCI_DEVID(dev->bus->number, dev->devfn);
}

static int pci_stub_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	unsigned i;

	for (i = 0; i < num_except; i++)
		if (exception_matches(&except[i], dev)) {
			dev_info(&dev->dev, "skipped by stub\n");
			return -EPERM;
		}

	dev_info(&dev->dev, "claimed by stub\n");
	return 0;
}

static struct pci_driver stub_driver = {
	.name		= "pci-stub",
	.id_table	= NULL,	/* only dynamic id's */
	.probe		= pci_stub_probe,
};

static int __init pci_stub_init(void)
{
	char *p, *id;
	int rc;

	rc = pci_register_driver(&stub_driver);
	if (rc)
		return rc;

	/* parse exceptions */
	p = except_str;
	while ((id = strsep(&p, ","))) {
		int fields;
		unsigned domain, bus, dev, fn;

		if (*id == '\0')
			continue;

		fields = sscanf(id, "%x:%x:%x.%x", &domain, &bus, &dev, &fn);
		if (fields != 4 || domain > 0xffff || bus > 0xff ||
		    dev > 0x1f || fn > 0x7) {
			printk(KERN_WARNING
			       "pci-stub: invalid exception \"%s\"\n", id);
			continue;
		}

		if (num_except < MAX_EXCEPT) {
			struct except *ex = &except[num_except++];

			ex->domain = domain;
			ex->devid = PCI_DEVID(bus, PCI_DEVFN(dev, fn));
		} else
			printk(KERN_WARNING
			       "pci-stub: no room for exception \"%s\"\n", id);
	}

	/* no ids passed actually */
	if (ids[0] == '\0')
		return 0;

	/* add ids specified in the module parameter */
	p = ids;
	while ((id = strsep(&p, ","))) {
		unsigned int vendor, device, subvendor = PCI_ANY_ID,
			subdevice = PCI_ANY_ID, class = 0, class_mask = 0;
		int fields;

		if (!strlen(id))
			continue;

		fields = sscanf(id, "%x:%x:%x:%x:%x:%x",
				&vendor, &device, &subvendor, &subdevice,
				&class, &class_mask);

		if (fields < 2) {
			printk(KERN_WARNING
			       "pci-stub: invalid id string \"%s\"\n", id);
			continue;
		}

		printk(KERN_INFO
		       "pci-stub: add %04X:%04X sub=%04X:%04X cls=%08X/%08X\n",
		       vendor, device, subvendor, subdevice, class, class_mask);

		rc = pci_add_dynid(&stub_driver, vendor, device,
				   subvendor, subdevice, class, class_mask, 0);
		if (rc)
			printk(KERN_WARNING
			       "pci-stub: failed to add dynamic id (%d)\n", rc);
	}

	return 0;
}

static void __exit pci_stub_exit(void)
{
	pci_unregister_driver(&stub_driver);
}

module_init(pci_stub_init);
module_exit(pci_stub_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chris Wright <chrisw@sous-sol.org>");
