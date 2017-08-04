/*
 * vga_default.c: What is the default/boot PCI VGA device?
 *
 * (C) Copyright 2005 Benjamin Herrenschmidt <benh@kernel.crashing.org>
 * (C) Copyright 2007 Paulo R. Zanoni <przanoni@gmail.com>
 * (C) Copyright 2007, 2009 Tiago Vignatti <vignatti@freedesktop.org>
 * (C) Copyright 2017 Canonical Ltd. (Author: Daniel Axtens <dja@axtens.net>)
 *
 * (License from vgaarb.c)
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS
 * IN THE SOFTWARE.
 */

/*
 * What device should a graphics system draw to? In order of priority:
 *
 *  1) Any devices configured specifically by the user (think
 *     xorg.conf).
 *
 *  2) If the platform has a concept of a boot device for early boot
 *     messages (think BIOS displays on x86), that device.
 *
 *  3) If the platform does not have the concept of a boot device,
 *     then we still want to pick something. For now, pick the first
 *     PCI VGA device with a driver bound and with memory or I/O
 *     control on.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/init.h>

#include <linux/vga_default.h>

static struct pci_dev *vga_default;
/*
 * only go active after the late initcall so as not to interfere with
 * the arbiter
 */
static bool vga_default_active = false;

/**
 * vga_default_device - return the default VGA device
 *
 * This can be defined by the platform. The default implementation
 * is rather dumb and will probably only work properly on single
 * vga card setups and/or x86 platforms.
 *
 * If your VGA default device is not PCI, you'll have to return
 * NULL here. In this case, I assume it will not conflict with
 * any PCI card. If this is not true, I'll have to define two archs
 * hooks for enabling/disabling the VGA default device if that is
 * possible. This may be a problem with real _ISA_ VGA cards, in
 * addition to a PCI one. I don't know at this point how to deal
 * with that card. Can theirs IOs be disabled at all ? If not, then
 * I suppose it's a matter of having the proper arch hook telling
 * us about it, so we basically never allow anybody to succeed a
 * vga_get()...
 */

struct pci_dev *vga_default_device(void)
{
	return vga_default;
}
EXPORT_SYMBOL_GPL(vga_default_device);

void vga_set_default_device(struct pci_dev *pdev)
{
	if (vga_default == pdev)
		return;

	pci_dev_put(vga_default);
	vga_default = pci_dev_get(pdev);
}

static bool vga_default_try_device(struct pci_dev *pdev)
{
	u16 cmd;

	/* Only deal with VGA class devices */
	if ((pdev->class >> 8) != PCI_CLASS_DISPLAY_VGA)
		return false;

	/* Only deal with devices with drivers bound */
	if (!pdev->driver)
		return false;

	/* Require I/O or memory control */
	pci_read_config_word(pdev, PCI_COMMAND, &cmd);
	if (!(cmd & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY)))
		return false;

	dev_info(&pdev->dev, "vga_default: setting as default device\n");
	vga_set_default_device(pdev);
	return true;
}

static int __init vga_default_init(void)
{
	struct pci_dev *pdev;

	vga_default_active = true;

	if (vga_default_device())
		return 0;

	pdev = NULL;
	while ((pdev =
		pci_get_subsys(PCI_ANY_ID, PCI_ANY_ID, PCI_ANY_ID,
			       PCI_ANY_ID, pdev)) != NULL) {
		if (vga_default_try_device(pdev))
			return 0;
	}

	return 0;
}
late_initcall(vga_default_init);

/*
 * A driver could be loaded much later than late_initcall, for example
 * if it's in a module.
 *
 * We want to pick that up. However, we want to make sure this does
 * not interfere with the arbiter - it should only activate if the
 * arbiter has already had a chance to operate. To ensure this, we set
 * vga_default_active in the late_initcall: as the vga arbiter is a
 * subsys initcall, it is guaranteed to fire first.
 */
static void vga_default_enable_hook(struct pci_dev *pdev)
{
       if (!vga_default_active)
	       return;

       if (vga_default_device())
               return;

       vga_default_try_device(pdev);
}
DECLARE_PCI_FIXUP_CLASS_ENABLE(PCI_ANY_ID, PCI_ANY_ID,
			       PCI_CLASS_DISPLAY_VGA, 8,
			       vga_default_enable_hook)
