/*
 * Cisco ASR1K platform PCIe AER support
 *
 * Copyright (c) 2015 by cisco Systems, Inc.
 */

#include <linux/pci.h>
#include <../../../drivers/pci/pcie/aer/aerdrv.h>

/* MCH 5100 */
#define PCI_DEVICE_ID_5100_PORT_0   	0x7270
#define PCI_DEVICE_ID_5100_PORT_2_3	0x65F7
#define PCI_DEVICE_ID_5100_PORT_6	0x65E6
#define PEXCTRL3			0x4D
#define PEXCTRL3_MSI_RAS_ERREN		0x01
#define PEXCTRL				0x48
#define PEXCTRL_DIS_APIC_EOI		0x02

/* Jasper Forest - 3500/5500 Series */
#define PCI_DEVICE_ID_3500_PORT_1	0x3721
#define PCI_DEVICE_ID_3500_PORT_2	0x3722
#define PCI_DEVICE_ID_3500_PORT_3	0x3723
#define PCI_DEVICE_ID_3500_PORT_4	0x3724
#define MISCCTRLSTS_REG			0x188
#define MISCCTRLSTS_DISABLE_EOI_MASK	0x04000000

static int aer_err_src_mch5100(struct pci_dev *pdev, unsigned int *id)
{
	/*
	 * MCH 5100 doesn't populate src register, explained
	 * in an errata, so hard coding the source id
	 */
	*id = ((pdev->devfn << 16) | pdev->devfn);

	return 0;
}

static int aer_err_src_jf(struct pci_dev *pdev, unsigned int *id)
{
	/*
	 * Xeon 3500/5500 series (Jasper Forest) doesn't populate
	 * the source register either, so we must hard code.
	 */
	unsigned int devfn = (pdev->subordinate->number << 8) |
				PCI_DEVFN(0,0);
	*id = (devfn << 16) | devfn;

	return 0;
}

int aer_err_src(struct pci_dev *dev, unsigned int *id)
{
	if ((dev->vendor == PCI_VENDOR_ID_INTEL) &&
	    ((dev->device == PCI_DEVICE_ID_5100_PORT_0) ||
	     (dev->device == PCI_DEVICE_ID_5100_PORT_2_3) ||
	     (dev->device == PCI_DEVICE_ID_5100_PORT_6)))
		return aer_err_src_mch5100(dev, id);

	if ((dev->vendor == PCI_VENDOR_ID_INTEL) &&
	    ((dev->device == PCI_DEVICE_ID_3500_PORT_1) ||
	     (dev->device == PCI_DEVICE_ID_3500_PORT_2) ||
	     (dev->device == PCI_DEVICE_ID_3500_PORT_3) ||
	     (dev->device == PCI_DEVICE_ID_3500_PORT_4)))
		return aer_err_src_jf(dev, id);

	return 0;
}

static bool aer_callbacks_set;

static struct pci_aer_callbacks aer_callbacks = {
	.error_source = aer_err_src,
};

static void aer_enable_rootport_mch5100(struct pci_dev *pdev)
{
	u32 reg32;
	u8 reg8;

	if (!aer_callbacks_set) {
		pci_aer_set_callbacks(&aer_callbacks);
		aer_callbacks_set = true;
	}

	/*
	 * MCH5100 sends broadcast EOI to subordinate
	 * devices. It is a vendor (Intel) specific message
	 * that should be ignored by non-Intel devices, but
	 * our devices (Hytop etc) do not ignore it and
	 * raise Uncorrectable and Unsupported Request
	 * errors.
	 *
	 * The EOI is for the Intel IO APIC, which is not
	 * present and therefore not required.
	 *
	 * Disable EOI Broadcast to avoid Uncorrectable and
	 * Unsupported request errors from devices which do
	 * not support the EOI and do not adhere to the PCIe
	 * spec.
	 */
	pci_read_config_dword(pdev, PEXCTRL, &reg32);
	reg32 |= PEXCTRL_DIS_APIC_EOI;
	pci_write_config_dword(pdev, PEXCTRL, reg32);

	/* Enable MSI */
	pci_read_config_byte(pdev, PEXCTRL3, &reg8);
	reg8 |= PEXCTRL3_MSI_RAS_ERREN;
	pci_write_config_byte(pdev, PEXCTRL3, reg8);
}

static void aer_enable_rootport_jf(struct pci_dev *pdev)
{
	u32 reg32;
	u16 reg16;

	if (!aer_callbacks_set) {
		pci_aer_set_callbacks(&aer_callbacks);
		aer_callbacks_set = true;
	}

	/*
	 * 3500/5500 series CPUs (JF) send broadcast EOI to
	 * subordinate devices. It is a vendor (Intel) specific
	 * message that should be ignored by non-Intel devices,
	 * but our devices (Yoda etc) do not ignore it and
	 * raise Uncorrectable and Unsupported Request
	 * errors.
	 *
	 * The EOI is for the Intel IO APIC, which is not
	 * present and therefore not required.
	 *
	 * Disable EOI Broadcast to avoid Uncorrectable and
	 * Unsupported request errors from devices which do
	 * not support the EOI and do not adhere to the PCIe
	 * spec.
	 */
	pci_read_config_dword(pdev, MISCCTRLSTS_REG, &reg32);
	reg32 |= MISCCTRLSTS_DISABLE_EOI_MASK;
	pci_write_config_dword(pdev, MISCCTRLSTS_REG, reg32);

	/*
	 * We must also forward #SERR and #PERR from the secondary
	 * to primary bus.  This will result in the AER driver
	 * receiving an interrupt that can then be delivered to
	 * the device specific driver.
	 */
	pci_read_config_word(pdev, PCI_BRIDGE_CONTROL, &reg16);
	reg16 |= PCI_BRIDGE_CTL_PARITY | PCI_BRIDGE_CTL_SERR;
	pci_write_config_word(pdev, PCI_BRIDGE_CONTROL, reg16);
}

DECLARE_PCI_FIXUP_AER_ENABLE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_5100_PORT_0,
				aer_enable_rootport_mch5100);
DECLARE_PCI_FIXUP_AER_ENABLE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_5100_PORT_2_3,
				aer_enable_rootport_mch5100);
DECLARE_PCI_FIXUP_AER_ENABLE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_5100_PORT_6,
				aer_enable_rootport_mch5100);
DECLARE_PCI_FIXUP_AER_ENABLE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_3500_PORT_1,
				aer_enable_rootport_jf);
DECLARE_PCI_FIXUP_AER_ENABLE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_3500_PORT_2,
				aer_enable_rootport_jf);
DECLARE_PCI_FIXUP_AER_ENABLE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_3500_PORT_3,
				aer_enable_rootport_jf);
DECLARE_PCI_FIXUP_AER_ENABLE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_3500_PORT_4,
				aer_enable_rootport_jf);
