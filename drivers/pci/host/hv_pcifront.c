/*
 * Copyright (c) Microsoft Corporation.
 *
 * Author:
 *   Jake Oshins <jakeo@microsoft.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/semaphore.h>
#include <linux/irqdomain.h>
#include <asm/irqdomain.h>
#include <linux/msi.h>
#include <linux/hyperv.h>
#include <asm/mshyperv.h>

/*
 * Protocol versions. The low word is the minor version, the high word the major
 * version.
 */

#define PCI_MAKE_VERSION(major, minor) ((__u32)(((major) << 16) | (major)))
#define PCI_MAJOR_VERSION(version) ((__u32)(version) >> 16)
#define PCI_MINOR_VERSION(version) ((__u32)(version) & 0xff)

enum {
	PCI_PROTOCOL_VERSION_1_1 = PCI_MAKE_VERSION(1, 1),
	PCI_PROTOCOL_VERSION_CURRENT = PCI_PROTOCOL_VERSION_1_1
};

#define PCI_CONFIG_MMIO_LENGTH	0x2000
#define MAX_SUPPORTED_MSI_MESSAGES 0x400

/*
 * Message Types
 */

enum pci_message_type {
	/*
	 * Version 1.1
	 */
	PCI_MESSAGE_BASE                = 0x42490000,
	PCI_BUS_RELATIONS               = PCI_MESSAGE_BASE + 0,
	PCI_QUERY_BUS_RELATIONS         = PCI_MESSAGE_BASE + 1,
	PCI_POWER_STATE_CHANGE          = PCI_MESSAGE_BASE + 4,
	PCI_QUERY_RESOURCE_REQUIREMENTS = PCI_MESSAGE_BASE + 5,
	PCI_QUERY_RESOURCE_RESOURCES    = PCI_MESSAGE_BASE + 6,
	PCI_BUS_D0ENTRY                 = PCI_MESSAGE_BASE + 7,
	PCI_BUS_D0EXIT                  = PCI_MESSAGE_BASE + 8,
	PCI_READ_BLOCK                  = PCI_MESSAGE_BASE + 9,
	PCI_WRITE_BLOCK                 = PCI_MESSAGE_BASE + 0xA,
	PCI_EJECT                       = PCI_MESSAGE_BASE + 0xB,
	PCI_QUERY_STOP                  = PCI_MESSAGE_BASE + 0xC,
	PCI_REENABLE                    = PCI_MESSAGE_BASE + 0xD,
	PCI_QUERY_STOP_FAILED           = PCI_MESSAGE_BASE + 0xE,
	PCI_EJECTION_COMPLETE           = PCI_MESSAGE_BASE + 0xF,
	PCI_RESOURCES_ASSIGNED          = PCI_MESSAGE_BASE + 0x10,
	PCI_RESOURCES_RELEASED          = PCI_MESSAGE_BASE + 0x11,
	PCI_INVALIDATE_BLOCK            = PCI_MESSAGE_BASE + 0x12,
	PCI_QUERY_PROTOCOL_VERSION      = PCI_MESSAGE_BASE + 0x13,
	PCI_CREATE_INTERRUPT_MESSAGE    = PCI_MESSAGE_BASE + 0x14,
	PCI_DELETE_INTERRUPT_MESSAGE    = PCI_MESSAGE_BASE + 0x15,
	PCI_MESSAGE_MAXIMUM
};

/*
 * Structures defining the virtual PCI Express protocol.
 */

union pci_version {
	struct {
		__u16 minor_version;
		__u16 major_version;
	} parts;
	__u32 version;
} __packed;

/*
 * Function numbers are 8-bits wide on Express, as interpreted through ARI,
 * which is all this driver does.  This representation is the one used in
 * Windows, which is what is expected when sending this back and forth with
 * the Hyper-V parent partition.
 */
union win_slot_encoding {
	struct {
		__u32   func:8;
		__u32   reserved:24;
	} bits;
	__u32 slot;
} __packed;

/*
 * Pretty much as defined in the PCI Specifications.
 */
struct pci_function_description {
	__u16   v_id;	/* vendor ID */
	__u16   d_id;	/* device ID */
	__u8    rev;
	__u8    prog_intf;
	__u8    subclass;
	__u8    base_class;
	__u32   subsystem_id;
	union win_slot_encoding win_slot;
	__u32   ser;	/* serial number */
} __packed;

/**
 * struct hv_msi_desc
 * @vector:		IDT entry
 * @delivery_mode:	As defined in Intel's Programmer's
 *			Reference Manual, Volume 3, Chapter 8.
 * @vector_count:	Number of contiguous entries in the
 *			Interrupt Descriptor Table that are
 *			occupied by this Message-Signaled
 *			Interrupt. For "MSI", as first defined
 *			in PCI 2.2, this can be between 1 and
 *			32. For "MSI-X," as first defined in PCI
 *			3.0, this must be 1, as each MSI-X table
 *			entry would have its own descriptor.
 * @reserved:		Empty space
 * @cpu_mask:		All the target virtual processors.
 */
struct hv_msi_desc {
	__u8	vector;
	__u8	delivery_mode;
	__u16	vector_count;
	__u32	reserved;
	__u64	cpu_mask;
} __packed;

/**
 * struct tran_int_desc
 * @reserved:		unused, padding
 * @vector_count:	same as in hv_msi_desc
 * @data:		This is the "data payload" value that is
 *			written by the device when it generates
 *			a message-signaled interrupt, either MSI
 *			or MSI-X.
 * @address:		This is the address to which the data
 *			payload is written on interrupt
 *			generation.
 */
struct tran_int_desc {
	__u16	reserved;
	__u16	vector_count;
	__u32	data;
	__u64	address;
} __packed;

/*
 * A generic message format for virtual PCI.
 * Specific message formats are defined later in the file.
 */

struct pci_message {
	__u32 message_type;
} __packed;

struct pci_child_message {
	__u32 message_type;
	union win_slot_encoding wslot;
} __packed;

struct pci_incoming_message {
	struct vmpacket_descriptor hdr;
	struct pci_message message_type;
} __packed;

struct pci_response {
	struct vmpacket_descriptor hdr;
	__s32 status; /* negative values are failures*/
} __packed;

struct pci_packet {
	void (*completion_func)(void *context, struct pci_response *resp,
				int resp_packet_size);
	void *compl_ctxt;
	struct pci_message message;
};

/*
 * Specific message types supporting the PCI protocol.
 */

/*
 * Version negotiation message. Sent from the guest to the host.
 * The guest is free to try different versions until the host
 * accepts the version.
 *
 * pci_version: The protocol version requested.
 * is_last_attempt: If TRUE, this is the last version guest will request.
 * reservedz: Reserved field, set to zero.
 */

struct pci_version_request {
	struct pci_message message_type;
	enum pci_message_type protocol_version;
} __packed;

/*
 * Bus D0 Entry.  This is sent from the guest to the host when the virtual
 * bus (PCI Express port) is ready for action.
 */

struct pci_bus_d0_entry {
	struct pci_message message_type;
	__u32 reserved;
	__u64 mmio_base;
} __packed;

struct pci_bus_relations {
	struct pci_incoming_message incoming;
	__u32 device_count;
	struct pci_function_description func[1];
} __packed;

struct pci_q_res_req_response {
	struct vmpacket_descriptor hdr;
	__s32 status; /* negative values are failures*/
	__u32 probed_bar[6];
} __packed;

struct pci_set_power {
	struct pci_message message_type;
	union win_slot_encoding wslot;
	__u32 power_state; /* In Windows terms */
	__u32 reserved;
} __packed;

struct pci_set_power_response {
	struct vmpacket_descriptor hdr;
	__s32 status; /* negative values are failures*/
	union win_slot_encoding wslot;
	__u32 resultant_state;  /* In Windows terms */
	__u32 reserved;
} __packed;

struct pci_resources_assigned {
	struct pci_message message_type;
	union win_slot_encoding wslot;
	u8 memory_range[0x14][6]; /* not used here */
	u32 msi_descriptors;
	u32 reserved[4];
} __packed;

struct pci_create_interrupt {
	struct pci_message message_type;
	union win_slot_encoding wslot;
	struct hv_msi_desc int_desc;
} __packed;

struct pci_create_int_response {
	struct pci_response response;
	u32 reserved;
	struct tran_int_desc int_desc;
} __packed;

struct pci_delete_interrupt {
	struct pci_message message_type;
	union win_slot_encoding wslot;
	struct tran_int_desc int_desc;
} __packed;

struct pci_dev_incoming {
	struct pci_incoming_message incoming;
	union win_slot_encoding wslot;
} __packed;

struct pci_eject_response {
	u32 message_type;
	union win_slot_encoding wslot;
	u32 status;
} __packed;

static int pci_ring_size = (4 * PAGE_SIZE);

/*
 * Definitions or interrupt steering hypercall.
 */
#define HV_PARTITION_ID_SELF		((u64)-1)
#define HVCALL_RETARGET_INTERRUPT	0x7e

struct retarget_msi_interrupt {
	u64	partition_id; /* use "self" */
	u64	device_id;
	u32	source; /* 1 for MSI(-X) */
	u32	reserved1;
	u32	address;
	u32	data;
	u64	reserved2;
	u32	vector;
	u32	flags;
	u64	vp_mask;
} __packed;

/*
 * Driver specific state.
 */

enum hv_pcibus_state {
	hv_pcibus_init = 0,
	hv_pcibus_probed,
	hv_pcibus_installed,
	hv_pcibus_maximum
};

struct hv_pcibus_device {
	struct pci_sysdata sysdata;
	enum hv_pcibus_state state;
	atomic_t remove_lock;
	struct hv_device *hdev;
	resource_size_t low_mmio_space;
	resource_size_t high_mmio_space;
	struct resource *mem_config;
	struct resource *low_mmio_res;
	struct resource *high_mmio_res;
	struct completion *survey_event;
	struct completion remove_event;
	struct pci_bus *pci_bus;
	spinlock_t config_lock; /* Avoids two threads writing index page */
	spinlock_t device_list_lock; /* Protects lists below */
	void __iomem *cfg_addr;

	struct semaphore enum_sem;
	struct list_head resources_for_children;

	struct list_head children;
	struct list_head dr_list;
	struct work_struct wrk;

	struct msi_domain_info msi_info;
	struct msi_controller msi_chip;
	struct irq_domain *irq_domain;
};

/*
 * Tracks "Device Relations" messages from the host, which must be both
 * processed in order and deferred so that they don't run in the context
 * of the incoming packet callback.
 */
struct hv_dr_work {
	struct work_struct wrk;
	struct hv_pcibus_device *bus;
};

struct hv_dr_state {
	struct list_head list_entry;
	__u32 device_count;
	struct pci_function_description func[1];
};

enum hv_pcichild_state {
	hv_pcichild_init = 0,
	hv_pcichild_requirements,
	hv_pcichild_resourced,
	hv_pcichild_ejecting,
	hv_pcichild_maximum
};

enum hv_pcidev_ref_reason {
	hv_pcidev_ref_invalid = 0,
	hv_pcidev_ref_initial,
	hv_pcidev_ref_by_slot,
	hv_pcidev_ref_packet,
	hv_pcidev_ref_pnp,
	hv_pcidev_ref_childlist,
	hv_pcidev_irqdata,
	hv_pcidev_ref_max
};

static char *hv_pcidev_ref_debug[hv_pcidev_ref_max] = {
	"hv_pcidev_ref_invalid",
	"hv_pcidev_ref_initial",
	"hv_pcidev_ref_by_slot",
	"hv_pcidev_ref_packet",
	"hv_pcidev_ref_pnp",
	"hv_pcidev_ref_childlist",
	"hv_pcidev_irqdata"
};

struct hv_pci_dev {
	/* List protected by pci_rescan_remove_lock */
	struct list_head list_entry;
	atomic_t refs;
	enum hv_pcichild_state state;
	struct pci_function_description desc;
	bool reported_missing;
	struct hv_pcibus_device *hbus;
	struct work_struct wrk;

	/*
	 * What would be observed if one wrote 0xFFFFFFFF to a BAR and then
	 * read it back, for each of the BAR offsets within config space.
	 */
	u32 probed_bar[6];
};

struct hv_pci_compl {
	struct completion host_event;
	__s32 completion_status;
};

/**
 * hv_pci_generic_compl() - Invoked for a completion packet
 * @context:		Set up by the sender of the packet.
 * @resp:		The response packet
 * @resp_packet_size:	Size in bytes of the packet
 *
 * This function is used to trigger an event and report status
 * for any message for which the completion packet contains a
 * status and nothing else.
 */
static
void
hv_pci_generic_compl(void *context, struct pci_response *resp,
		     int resp_packet_size)
{
	struct hv_pci_compl *comp_pkt = context;

	if (resp_packet_size >= offsetofend(struct pci_response, status))
		comp_pkt->completion_status = resp->status;
	complete(&comp_pkt->host_event);
}

static struct hv_pci_dev *lookup_hv_dev(struct hv_pcibus_device *hbus,
					u32 wslot);
static void hv_pcichild_inc(struct hv_pci_dev *hv_pcidev,
			    enum hv_pcidev_ref_reason reason);
static void hv_pcichild_dec(struct hv_pci_dev *hv_pcidev,
			    enum hv_pcidev_ref_reason reason);

static void hv_pcibus_inc(struct hv_pcibus_device *hv_pcibus);
static void hv_pcibus_dec(struct hv_pcibus_device *hv_pcibus);

/**
 * devfn_to_wslot() - Convert from Linux PCI slot to Windows
 * @devfn:	The Linux representation of PCI slot
 *
 * Windows uses a slightly different representation of PCI slot.
 *
 * Return: The Windows representation
 */
static u32 devfn_to_wslot(int devfn)
{
	union win_slot_encoding wslot;

	wslot.slot = 0;
	wslot.bits.func = PCI_SLOT(devfn) | (PCI_FUNC(devfn) << 5);

	return wslot.slot;
}

/**
 * wslot_to_devfn() - Convert from Windows PCI slot to Linux
 * @wslot:	The Windows representation of PCI slot
 *
 * Windows uses a slightly different representation of PCI slot.
 *
 * Return: The Linux representation
 */
static int wslot_to_devfn(u32 wslot)
{
	union win_slot_encoding slot_no;

	slot_no.slot = wslot;
	return PCI_DEVFN(0, slot_no.bits.func);
}

/**
 * _hv_pcifront_read_config() - Internal PCI config read
 * @hpdev:	The PCI driver's representation of the device
 * @where:	Offset within config space
 * @size:	Size of the transfer
 * @val:	Pointer to the buffer receiving the data
 */
static void _hv_pcifront_read_config(struct hv_pci_dev *hpdev, int where,
				     int size, u32 *val)
{
	unsigned long flags;
	void __iomem *addr = hpdev->hbus->cfg_addr + 0x1000 + where;

	/*
	 * If the attempt is to read the IDs or the ROM BAR, simulate that.
	 */
	if (where + size <= PCI_COMMAND) {
		memcpy(val, ((u8 *)&hpdev->desc.v_id) + where, size);
	} else if (where >= PCI_CLASS_REVISION && where + size <=
		   PCI_CACHE_LINE_SIZE) {
		memcpy(val, ((u8 *)&hpdev->desc.rev) + where -
		       PCI_CLASS_REVISION, size);
	} else if (where >= PCI_SUBSYSTEM_VENDOR_ID && where + size <=
		   PCI_ROM_ADDRESS) {
		memcpy(val, (u8 *)&hpdev->desc.subsystem_id + where -
		       PCI_SUBSYSTEM_VENDOR_ID, size);
	} else if (where >= PCI_ROM_ADDRESS && where + size <=
		   PCI_CAPABILITY_LIST) {
		/* ROM BARs are unimplemented */
		*val = 0;
	} else if (where >= PCI_INTERRUPT_LINE && where + size <=
		   PCI_INTERRUPT_PIN) {
		/*
		 * Interrupt Line and Interrupt PIN are hard-wired to zero
		 * because this front-end only supports message-signaled
		 * interrupts.
		 */
		*val = 0;
	} else {
		spin_lock_irqsave(&hpdev->hbus->config_lock, flags);
		writel(hpdev->desc.win_slot.slot, hpdev->hbus->cfg_addr);
		switch (size) {
		case 1:
			*val = readb(addr);
			break;
		case 2:
			*val = readw(addr);
			break;
		default:
			*val = readl(addr);
			break;
		}
		spin_unlock_irqrestore(&hpdev->hbus->config_lock, flags);
	}
}

/**
 * _hv_pcifront_write_config() - Internal PCI config write
 * @hpdev:	The PCI driver's representation of the device
 * @where:	Offset within config space
 * @size:	Size of the transfer
 * @val:	The data being transferred
 */
static void _hv_pcifront_write_config(struct hv_pci_dev *hpdev, int where,
				      int size, u32 val)
{
	unsigned long flags;
	void __iomem *addr = hpdev->hbus->cfg_addr + 0x1000 + where;

	if (where >= PCI_SUBSYSTEM_VENDOR_ID &&
	    where + size <= PCI_CAPABILITY_LIST) {
		/* SSIDs and ROM BARs are read-only */
	} else if (where >= PCI_COMMAND) {
		spin_lock_irqsave(&hpdev->hbus->config_lock, flags);
		writel(hpdev->desc.win_slot.slot, hpdev->hbus->cfg_addr);
		switch (size) {
		case 1:
			writeb(val, addr);
			break;
		case 2:
			writew(val, addr);
			break;
		default:
			writel(val, addr);
			break;
		}
		spin_unlock_irqrestore(&hpdev->hbus->config_lock, flags);
	}
}

/**
 * hv_pcifront_read_config() - Read configuration space
 * @bus: PCI Bus structure
 * @devfn: Device/function
 * @where: Offset from base
 * @size: Byte/word/dword
 * @val: Value to be read
 *
 * Return: PCIBIOS_SUCCESSFUL on success
 *	   PCIBIOS_DEVICE_NOT_FOUND on failure
 */
static int hv_pcifront_read_config(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 *val)
{
	struct hv_pcibus_device *hbus =
		container_of(bus->sysdata, struct hv_pcibus_device, sysdata);
	struct hv_pci_dev *hpdev;

	hpdev = lookup_hv_dev(hbus, devfn_to_wslot(devfn));
	if (!hpdev)
		return PCIBIOS_DEVICE_NOT_FOUND;

	_hv_pcifront_read_config(hpdev, where, size, val);

	hv_pcichild_dec(hpdev, hv_pcidev_ref_by_slot);
	return PCIBIOS_SUCCESSFUL;
}

/**
 * hv_pcifront_write_config() - Write configuration space
 * @bus: PCI Bus structure
 * @devfn: Device/function
 * @where: Offset from base
 * @size: Byte/word/dword
 * @val: Value to be written to device
 *
 * Return: PCIBIOS_SUCCESSFUL on success
 *	   PCIBIOS_DEVICE_NOT_FOUND on failure
 */
static int hv_pcifront_write_config(struct pci_bus *bus, unsigned int devfn,
				    int where, int size, u32 val)
{
	struct hv_pcibus_device *hbus =
	    container_of(bus->sysdata, struct hv_pcibus_device, sysdata);
	struct hv_pci_dev *hpdev;

	hpdev = lookup_hv_dev(hbus, devfn_to_wslot(devfn));
	if (!hpdev)
		return PCIBIOS_DEVICE_NOT_FOUND;

	_hv_pcifront_write_config(hpdev, where, size, val);

	hv_pcichild_dec(hpdev, hv_pcidev_ref_by_slot);
	return PCIBIOS_SUCCESSFUL;
}

/* PCIe operations */
static struct pci_ops hv_pcifront_ops = {
	.read  = hv_pcifront_read_config,
	.write = hv_pcifront_write_config,
};

/* Interrupt management hooks */
static void hv_int_desc_free(struct hv_pci_dev *hpdev,
			     struct tran_int_desc *int_desc)
{
	struct pci_delete_interrupt *int_pkt;
	struct {
		struct pci_packet pkt;
		u8 buffer[sizeof(struct pci_delete_interrupt) -
			  sizeof(struct pci_message)];
	} ctxt;

	memset(&ctxt, 0, sizeof(ctxt));
		int_pkt = (struct pci_delete_interrupt *)&ctxt.pkt.message;
		int_pkt->message_type.message_type =
			PCI_DELETE_INTERRUPT_MESSAGE;
		int_pkt->wslot.slot = hpdev->desc.win_slot.slot;
		int_pkt->int_desc = *int_desc;
		vmbus_sendpacket(hpdev->hbus->hdev->channel, int_pkt,
				 sizeof(*int_pkt),
				 (unsigned long)&ctxt.pkt, VM_PKT_DATA_INBAND,
				 0);
	kfree(int_desc);
}

/**
 * hv_msi_free() - Free the MSI.
 * @domain:	The interrupt domain pointer
 * @info:	Extra MSI-related context
 * @irq:	Identifies the IRQ.
 *
 * The Hyper-V parent partition and hypervisor are tracking the
 * messages that are in use, keeping the interrupt redirection
 * table up to date.  This callback sends a message that frees
 * the the IRT entry and related tracking nonsense.
 */
static void hv_msi_free(struct irq_domain *domain, struct msi_domain_info *info,
			unsigned int irq)
{
	struct hv_pcibus_device *hbus;
	struct hv_pci_dev *hpdev;
	struct pci_dev *pdev;
	struct tran_int_desc *int_desc;
	struct irq_data *irq_data = irq_domain_get_irq_data(domain, irq);
	struct msi_desc *msi = irq_data_get_msi_desc(irq_data);

	pdev = msi_desc_to_pci_dev(msi);
	hbus = info->data;
	hpdev = lookup_hv_dev(hbus, devfn_to_wslot(pdev->devfn));
	if (!hpdev)
		return;

	int_desc = irq_data_get_irq_chip_data(irq_data);
	if (int_desc) {
		irq_data->chip_data = NULL;
		hv_int_desc_free(hpdev, int_desc);
	}

	hv_pcichild_dec(hpdev, hv_pcidev_ref_by_slot);
}

static int hv_set_affinity(struct irq_data *data, const struct cpumask *dest,
			   bool force)
{
	struct irq_data *parent = data->parent_data;

	return parent->chip->irq_set_affinity(parent, dest, force);
}

void hv_irq_mask(struct irq_data *data)
{
	pci_msi_mask_irq(data);
}

/**
 * hv_irq_unmask() - "Unmask" the IRQ by setting its current
 * affinity.
 * @data:	Describes the IRQ
 *
 * Build new a destination for the MSI and make a hypercall to
 * update the Interrupt Redirection Table. "Device Logical ID"
 * is built out of this PCI bus's instance GUID and the function
 * number of the device.
 */
void hv_irq_unmask(struct irq_data *data)
{
	struct msi_desc *msi_desc = irq_data_get_msi_desc(data);
	struct irq_cfg *cfg = irqd_cfg(data);
	struct retarget_msi_interrupt params;
	struct hv_pcibus_device *hbus;
	struct cpumask *dest;
	struct pci_bus *pbus;
	struct pci_dev *pdev;
	int cpu;

	dest = irq_data_get_affinity_mask(data);
	pdev = msi_desc_to_pci_dev(msi_desc);
	pbus = pdev->bus;
	hbus = container_of(pbus->sysdata, struct hv_pcibus_device, sysdata);

	memset(&params, 0, sizeof(params));
	params.partition_id = HV_PARTITION_ID_SELF;
	params.source = 1; /* MSI(-X) */
	params.address = msi_desc->msg.address_lo;
	params.data = msi_desc->msg.data;
	params.device_id = (hbus->hdev->dev_instance.b[5] << 24) |
			   (hbus->hdev->dev_instance.b[4] << 16) |
			   (hbus->hdev->dev_instance.b[7] << 8) |
			   (hbus->hdev->dev_instance.b[6] & 0xf8) |
			   PCI_FUNC(pdev->devfn);
	params.vector = cfg->vector;

	for_each_cpu_and(cpu, dest, cpu_online_mask)
		params.vp_mask |= (1ULL << vmbus_cpu_number_to_vp_number(cpu));

	hv_do_hypercall(HVCALL_RETARGET_INTERRUPT, &params, NULL);

	pci_msi_unmask_irq(data);
}

struct compose_comp_ctxt {
	struct hv_pci_compl comp_pkt;
	struct tran_int_desc int_desc;
};

static void hv_pci_compose_compl(void *context, struct pci_response *resp,
				 int resp_packet_size)
{
	struct compose_comp_ctxt *comp_pkt = context;
	struct pci_create_int_response *int_resp =
		(struct pci_create_int_response *)resp;

	comp_pkt->comp_pkt.completion_status = resp->status;
	comp_pkt->int_desc = int_resp->int_desc;
	complete(&comp_pkt->comp_pkt.host_event);
}

/**
 * hv_compose_msi_msg() - Supplies a valid MSI address/data
 * @data:	Everything about this MSI
 * @msg:	Buffer that is filled in by this function
 *
 * This function unpacks the IRQ looking for target CPU set, IDT
 * vector and mode and sends a message to the parent partition
 * asking for a mapping for that tuple in this partition.  The
 * response supplies a data value and address to which that data
 * should be written to trigger that interrupt.
 */
static void hv_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct irq_cfg *cfg = irqd_cfg(data);
	struct hv_pcibus_device *hbus;
	struct hv_pci_dev *hpdev;
	struct pci_bus *pbus;
	struct pci_dev *pdev;
	struct pci_create_interrupt *int_pkt;
	struct compose_comp_ctxt comp;
	struct tran_int_desc *int_desc;
	struct cpumask *affinity;
	struct {
		struct pci_packet pkt;
		u8 buffer[sizeof(struct pci_create_interrupt) -
			  sizeof(struct pci_message)];
	} ctxt;
	int cpu;
	int ret;

	pdev = msi_desc_to_pci_dev(irq_data_get_msi_desc(data));
	pbus = pdev->bus;
	hbus = container_of(pbus->sysdata, struct hv_pcibus_device, sysdata);
	hpdev = lookup_hv_dev(hbus, devfn_to_wslot(pdev->devfn));

	if (!hpdev)
		goto return_null_message;

	/* Free any previous message that might have already been composed. */
	if (data->chip_data) {
		int_desc = data->chip_data;
		data->chip_data = NULL;
		hv_int_desc_free(hpdev, int_desc);
	}

	int_desc = kzalloc(sizeof(*int_desc), GFP_KERNEL);
	if (!int_desc)
		goto drop_reference;

	memset(&ctxt, 0, sizeof(ctxt));
	init_completion(&comp.comp_pkt.host_event);
	ctxt.pkt.completion_func = hv_pci_compose_compl;
	ctxt.pkt.compl_ctxt = &comp;
	int_pkt = (struct pci_create_interrupt *)&ctxt.pkt.message;
	int_pkt->message_type.message_type = PCI_CREATE_INTERRUPT_MESSAGE;
	int_pkt->wslot.slot = hpdev->desc.win_slot.slot;
	int_pkt->int_desc.vector = cfg->vector;
	int_pkt->int_desc.vector_count = 1;
	int_pkt->int_desc.delivery_mode =
		(apic->irq_delivery_mode == dest_LowestPrio) ? 1 : 0;

	/*
	 * This bit doesn't have to work on machines with more than 64
	 * processors because Hyper-V only supports 64 in a guest.
	 */
	affinity = irq_data_get_affinity_mask(data);
	for_each_cpu_and(cpu, affinity, cpu_online_mask) {
		int_pkt->int_desc.cpu_mask |=
			(1ULL << vmbus_cpu_number_to_vp_number(cpu));
	}

	ret = vmbus_sendpacket(hpdev->hbus->hdev->channel, int_pkt,
			       sizeof(*int_pkt), (unsigned long)&ctxt.pkt,
			       VM_PKT_DATA_INBAND,
			       VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (!ret)
		wait_for_completion(&comp.comp_pkt.host_event);

	if (comp.comp_pkt.completion_status < 0) {
		pr_err("Request for interrupt failed: 0x%x",
		       comp.comp_pkt.completion_status);
		goto free_int_desc;
	}

	/*
	 * Record the assignment so that this can be unwound later. Using
	 * irq_set_chip_data() here would be appropriate, but the lock it takes
	 * is already held.
	 */
	*int_desc = comp.int_desc;
	data->chip_data = int_desc;

	/* Pass up the result. */
	msg->address_hi = comp.int_desc.address >> 32;
	msg->address_lo = comp.int_desc.address & 0xffffffff;
	msg->data = comp.int_desc.data;

	hv_pcichild_dec(hpdev, hv_pcidev_ref_by_slot);
	return;

free_int_desc:
	kfree(int_desc);
drop_reference:
	hv_pcichild_dec(hpdev, hv_pcidev_ref_by_slot);
return_null_message:
	msg->address_hi = 0;
	msg->address_lo = 0;
	msg->data = 0;
}

/* HW Interrupt Chip Descriptor */
static struct irq_chip hv_msi_irq_chip = {
	.name			= "Hyper-V PCIe MSI",
	.irq_compose_msi_msg	= hv_compose_msi_msg,
	.irq_set_affinity	= hv_set_affinity,
	.irq_ack		= irq_chip_ack_parent,
	.irq_mask		= hv_irq_mask,
	.irq_unmask		= hv_irq_unmask,
};

static irq_hw_number_t hv_msi_domain_ops_get_hwirq(struct msi_domain_info *info,
						   msi_alloc_info_t *arg)
{
	return arg->msi_hwirq;
}

static struct msi_domain_ops hv_msi_ops = {
	.get_hwirq	= hv_msi_domain_ops_get_hwirq,
	.msi_prepare	= pci_msi_prepare,
	.set_desc	= pci_msi_set_desc,
	.msi_free	= hv_msi_free,
};

/**
 * hv_pcie_init_irq_domain() - Initialize IRQ domain
 * @hbus:	The root PCI bus
 *
 * This function creates an IRQ domain which will be used for
 * interrupts from devices that have been passed through.  These
 * devices only support MSI and MSI-X, not line-based interrupts
 * or simulations of line-based interrupts through PCIe's
 * fabric-layer messages.  Because interrupts are remapped, we
 * can support multi-message MSI here.
 *
 * Return: '0' on success and error value on failure
 */
static int hv_pcie_init_irq_domain(struct hv_pcibus_device *hbus)
{
	hbus->msi_info.chip = &hv_msi_irq_chip;
	hbus->msi_info.ops = &hv_msi_ops;
	hbus->msi_info.flags = (MSI_FLAG_USE_DEF_DOM_OPS |
		MSI_FLAG_USE_DEF_CHIP_OPS | MSI_FLAG_MULTI_PCI_MSI |
		MSI_FLAG_PCI_MSIX);
	hbus->msi_info.handler = handle_edge_irq;
	hbus->msi_info.handler_name = "edge";
	hbus->msi_info.data = hbus;
	hbus->irq_domain = pci_msi_create_irq_domain(hbus->sysdata.fwnode,
						     &hbus->msi_info,
						     x86_vector_domain);
	if (!hbus->irq_domain) {
		pr_err("Failed to build an MSI IRQ domain\n");
		return -ENODEV;
	}

	return 0;
}

/**
 * get_bar_size() - Get the address space consumed by a BAR
 * @bar_val:	Value that a BAR returned after -1 was written
 *              to it.
 *
 * This function returns the size of the BAR, rounded up to 1
 * page.  It has to be rounded up because the hypervisor's page
 * table entry that maps the BAR into the VM can't specify an
 * offset within a page.  The invariant is that the hypervisor
 * must place any BARs of smaller than page length at the
 * beginning of a page.
 *
 * Return:	Size in bytes of the consumed MMIO space.
 */
static u64 get_bar_size(u64 bar_val)
{
	return round_up((1 + ~(bar_val & PCI_BASE_ADDRESS_MEM_MASK)),
			PAGE_SIZE);
}

/**
 * survey_child_resources() - Total all MMIO requirements
 * @hbus:	Root PCI bus, as understood by this driver
 */
static void survey_child_resources(struct hv_pcibus_device *hbus)
{
	struct list_head *iter;
	struct hv_pci_dev *hpdev;
	resource_size_t bar_size = 0;
	unsigned long flags;
	struct completion *event;
	u64 bar_val;
	int i;

	/* If nobody is waiting on the answer, don't compute it. */
	event = xchg(&hbus->survey_event, NULL);
	if (!event)
		return;

	/* If the answer has already been computed, go with it. */
	if (hbus->low_mmio_space || hbus->high_mmio_space) {
		complete(event);
		return;
	}

	spin_lock_irqsave(&hbus->device_list_lock, flags);

	/*
	 * Due to an interesting quirk of the PCI spec, all memory regions
	 * for a child device are a power of 2 in size and aligned in memory,
	 * so it's sufficient to just add them up without tracking alignment.
	 */
	list_for_each(iter, &hbus->children) {
		hpdev = container_of(iter, struct hv_pci_dev, list_entry);
		for (i = 0; i < 6; i++) {
			if (hpdev->probed_bar[i] & PCI_BASE_ADDRESS_SPACE_IO)
				pr_err("There's an I/O BAR in this list!\n");

			if (hpdev->probed_bar[i] != 0) {
				/*
				 * A probed BAR has all the upper bits set that
				 * can be changed.
				 */

				bar_val = hpdev->probed_bar[i];
				if (bar_val & PCI_BASE_ADDRESS_MEM_TYPE_64)
					bar_val |=
					((u64)hpdev->probed_bar[++i] << 32);
				else
					bar_val |= 0xffffffff00000000ULL;

				bar_size = get_bar_size(bar_val);

				if (bar_val & PCI_BASE_ADDRESS_MEM_TYPE_64)
					hbus->high_mmio_space += bar_size;
				else
					hbus->low_mmio_space += bar_size;
			}
		}
	}

	spin_unlock_irqrestore(&hbus->device_list_lock, flags);
	complete(event);
}

/**
 * prepopulate_bars() - Fill in BARs with defaults
 * @hbus:	Root PCI bus, as understood by this driver
 *
 * The core PCI driver code seems much, much happier if the BARs
 * for a device have values upon first scan. So fill them in.
 * The algorithm below works down from large sizes to small,
 * attempting to pack the assignments optimally. The assumption,
 * enforced in other parts of the code, is that the beginning of
 * the memory-mapped I/O space will be aligned on the largest
 * BAR size.
 */
static void prepopulate_bars(struct hv_pcibus_device *hbus)
{
	resource_size_t high_size = 0;
	resource_size_t low_size = 0;
	resource_size_t high_base = 0;
	resource_size_t low_base = 0;
	resource_size_t bar_size;
	struct hv_pci_dev *hpdev;
	struct list_head *iter;
	unsigned long flags;
	u64 bar_val;
	u32 command;
	bool high;
	int i;

	if (hbus->low_mmio_space) {
		low_size = 1ULL << (63 - __builtin_clzll(hbus->low_mmio_space));
		low_base = hbus->low_mmio_res->start;
	}

	if (hbus->high_mmio_space) {
		high_size = 1ULL <<
			(63 - __builtin_clzll(hbus->high_mmio_space));
		high_base = hbus->high_mmio_res->start;
	}

	spin_lock_irqsave(&hbus->device_list_lock, flags);

	/* Pick addresses for the BARs. */
	do {
		list_for_each(iter, &hbus->children) {
			hpdev = container_of(iter, struct hv_pci_dev,
					     list_entry);
			for (i = 0; i < 6; i++) {
				bar_val = hpdev->probed_bar[i];
				if (bar_val == 0)
					continue;
				high = bar_val & PCI_BASE_ADDRESS_MEM_TYPE_64;
				if (high) {
					bar_val |=
						((u64)hpdev->probed_bar[i + 1]
						 << 32);
				} else {
					bar_val |= 0xffffffffULL << 32;
				}
				bar_size = get_bar_size(bar_val);
				if (high) {
					if (high_size != bar_size) {
						i++;
						continue;
					}
					_hv_pcifront_write_config(hpdev,
						PCI_BASE_ADDRESS_0 + (4 * i),
						4,
						(u32)(high_base & 0xffffff00));
					i++;
					_hv_pcifront_write_config(hpdev,
						PCI_BASE_ADDRESS_0 + (4 * i),
						4, (u32)(high_base >> 32));
					high_base += bar_size;
				} else {
					if (low_size != bar_size)
						continue;
					_hv_pcifront_write_config(hpdev,
						PCI_BASE_ADDRESS_0 + (4 * i),
						4,
						(u32)(low_base & 0xffffff00));
					low_base += bar_size;
				}
			}
			if (high_size <= 1 && low_size <= 1) {
				/* Set the memory enable bit. */
				_hv_pcifront_read_config(hpdev, PCI_COMMAND, 2,
							 &command);
				command |= PCI_COMMAND_MEMORY;
				_hv_pcifront_write_config(hpdev, PCI_COMMAND, 2,
							  command);
				break;
			}
		}

		high_size >>= 1;
		low_size >>= 1;
	}  while (high_size || low_size);

	spin_unlock_irqrestore(&hbus->device_list_lock, flags);
}

/**
 * create_root_hv_pci_bus() - Expose a new root PCI bus
 * @hbus:	Root PCI bus, as understood by this driver
 *
 * Return: 0 on success, -errno on failure
 */
static int create_root_hv_pci_bus(struct hv_pcibus_device *hbus)
{
	/* Register the device */
	hbus->pci_bus = pci_create_root_bus(&hbus->hdev->device,
					    0, /* bus number is always zero */
					    &hv_pcifront_ops,
					    &hbus->sysdata,
					    &hbus->resources_for_children);
	if (!hbus->pci_bus)
		return -ENODEV;

	hbus->pci_bus->msi = &hbus->msi_chip;
	hbus->pci_bus->msi->dev = &hbus->hdev->device;

	pci_scan_child_bus(hbus->pci_bus);
	pci_bus_assign_resources(hbus->pci_bus);
	pci_bus_add_devices(hbus->pci_bus);
	hbus->state = hv_pcibus_installed;
	return 0;
}

struct q_res_req_compl {
	struct completion host_event;
	struct hv_pci_dev *hpdev;
};

/**
 * q_resource_requirements() - Query Resource Requirements
 * @context:		The completion context.
 * @resp:		The respose that came from the host.
 * @resp_packet_size:	The size in bytes of resp.
 *
 * This function is invoked on completion of a Query Resource
 * Requirements packet.
 */
static void q_resource_requirements(void *context, struct pci_response *resp,
				    int resp_packet_size)
{
	struct q_res_req_compl *completion = context;
	struct pci_q_res_req_response *q_res_req =
		(struct pci_q_res_req_response *)resp;
	int i;

	if (resp->status < 0) {
		pr_err("query resource requirements failed: %x\n",
		       resp->status);
	} else {
		for (i = 0; i < 6; i++) {
			completion->hpdev->probed_bar[i] =
				q_res_req->probed_bar[i];
		}
	}

	complete(&completion->host_event);
}

static void hv_pcichild_inc(struct hv_pci_dev *hpdev,
			    enum hv_pcidev_ref_reason reason)
{
	pr_devel("+%s %p\n", hv_pcidev_ref_debug[reason], hpdev);
	atomic_inc(&hpdev->refs);
}

static void hv_pcichild_dec(struct hv_pci_dev *hpdev,
			    enum hv_pcidev_ref_reason reason)
{
	pr_devel("-%s %p\n", hv_pcidev_ref_debug[reason], hpdev);
	if (atomic_dec_and_test(&hpdev->refs)) {
		pr_devel("Freeing hv_pcidev %p\n", hpdev);
		kfree(hpdev);
	}
}

/**
 * new_pcichild_device() - Create a new child device
 * @hbus:	The internal struct tracking this root PCI bus.
 * @desc:	The information supplied so far from the host
 *              about the device.
 *
 * This function creates the tracking structure for a new child
 * device and kicks off the process of figuring out what it is.
 *
 * Return: Pointer to the new tracking struct
 */
static
struct hv_pci_dev *
new_pcichild_device(struct hv_pcibus_device *hbus,
		    struct pci_function_description *desc)
{
	struct hv_pci_dev *hpdev;
	struct pci_child_message *res_req;
	struct q_res_req_compl comp_pkt;
	union {
	struct pci_packet init_packet;
		__u8 buffer[0x100];
	} pkt;
	unsigned long flags;
	int ret;

	hpdev = kzalloc(sizeof(*hpdev), GFP_ATOMIC);
	if (!hpdev)
		return NULL;

	pr_devel("New child device (%p) reported (%04x:%04x) in slot %x.\n",
		 hpdev, desc->v_id, desc->d_id, desc->win_slot.slot);

	hpdev->hbus = hbus;

	memset(&pkt, 0, sizeof(pkt));
	init_completion(&comp_pkt.host_event);
	comp_pkt.hpdev = hpdev;
	pkt.init_packet.compl_ctxt = &comp_pkt;
	pkt.init_packet.completion_func = q_resource_requirements;
	res_req = (struct pci_child_message *)&pkt.init_packet.message;
	res_req->message_type = PCI_QUERY_RESOURCE_REQUIREMENTS;
	res_req->wslot.slot = desc->win_slot.slot;

	ret = vmbus_sendpacket(hbus->hdev->channel, res_req,
			       sizeof(struct pci_child_message),
			       (unsigned long)&pkt.init_packet,
			       VM_PKT_DATA_INBAND,
			       VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (ret)
		goto error;

	wait_for_completion(&comp_pkt.host_event);

	hpdev->desc = *desc;
	hv_pcichild_inc(hpdev, hv_pcidev_ref_initial);
	hv_pcichild_inc(hpdev, hv_pcidev_ref_childlist);
	spin_lock_irqsave(&hbus->device_list_lock, flags);
	list_add_tail(&hpdev->list_entry, &hbus->children);
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);
	return hpdev;

error:

	kfree(hpdev);
	return NULL;
}

/**
 * lookup_hv_dev() - Find device from slot
 * @hbus:	Root PCI bus, as understood by this driver
 * @wslot:	Location on the bus
 *
 * This function looks up a PCI device and returns in the
 * internal representation of it.  It leaves a reference on it,
 * so that the device won't be deleted while somebody is using
 * it.  The caller is responsible for dropping this reference.
 *
 * Return:	Internal representation of a PCI device
 */
static struct hv_pci_dev *lookup_hv_dev(struct hv_pcibus_device *hbus,
					u32 wslot)
{
	unsigned long flags;
	struct hv_pci_dev *iter, *hpdev = NULL;

	spin_lock_irqsave(&hbus->device_list_lock, flags);
	list_for_each_entry(iter, &hbus->children, list_entry) {
		if (iter->desc.win_slot.slot == wslot) {
			hpdev = iter;
			hv_pcichild_inc(hpdev, hv_pcidev_ref_by_slot);
			break;
		}
	}
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);

	return hpdev;
}

/**
 * pci_devices_present_work() - Handle new list of child devices
 * @work:	Work struct embedded in struct hv_dr_work
 *
 * "Bus Relations" is the Windows term for "children of this
 * bus."  The terminology is preserved here for people trying to
 * debug the interaction between Hyper-V and Linux.  This
 * function is called when the parent partition reports a list
 * of functions that should be observed under this PCI Express
 * port (bus).
 *
 * This function updates the list, and must tolerate being
 * called multiple times with the same information.  The typical
 * number of child devices is one, with very atypical cases
 * involving three or four, so the algorithms used here can be
 * simple and inefficient.
 *
 * It must also treat the omission of a previously observed device as
 * notification that the device no longer exists.
 *
 * Note that this function is a work item, and it may not be
 * invoked in the order that it was queued.  Back to back
 * updates of the list of present devices may involve queuing
 * mulitple work items, and this one may run before ones that
 * were sent later. As such, this function only does something
 * if is the last one in the queue.
 */
static void pci_devices_present_work(struct work_struct *work)
{
	u32 child_no;
	bool found;
	struct list_head *iter;
	struct pci_function_description *new_desc;
	struct hv_pci_dev *hpdev;
	struct hv_pcibus_device *hbus;
	struct list_head removed;
	struct hv_dr_work *dr_wrk;
	struct hv_dr_state *dr = NULL;
	unsigned long flags;

	dr_wrk = container_of(work, struct hv_dr_work, wrk);
	hbus = dr_wrk->bus;
	kfree(dr_wrk);

	INIT_LIST_HEAD(&removed);

	if (down_interruptible(&hbus->enum_sem)) {
		hv_pcibus_dec(hbus);
		return;
	}

	/* Pull this off the queue and process it if it was the last one. */
	spin_lock_irqsave(&hbus->device_list_lock, flags);
	while (!list_empty(&hbus->dr_list)) {
		dr = list_first_entry(&hbus->dr_list, struct hv_dr_state,
				      list_entry);
		list_del(&dr->list_entry);

		/* Throw this away if the list still has stuff in it. */
		if (!list_empty(&hbus->dr_list)) {
			kfree(dr);
			continue;
		}
	}
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);

	if (!dr) {
		up(&hbus->enum_sem);
		hv_pcibus_dec(hbus);
		return;
	}

	/* First, mark all existing children as reported missing. */
	spin_lock_irqsave(&hbus->device_list_lock, flags);
	list_for_each(iter, &hbus->children) {
			hpdev = container_of(iter, struct hv_pci_dev,
					     list_entry);
			hpdev->reported_missing = true;
	}
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);

	/* Next, add back any reported devices. */
	for (child_no = 0; child_no < dr->device_count; child_no++) {
		found = false;
		new_desc = &dr->func[child_no];

		spin_lock_irqsave(&hbus->device_list_lock, flags);
		list_for_each(iter, &hbus->children) {
			hpdev = container_of(iter, struct hv_pci_dev,
					     list_entry);
			if ((hpdev->desc.win_slot.slot ==
			     new_desc->win_slot.slot) &&
			    (hpdev->desc.v_id == new_desc->v_id) &&
			    (hpdev->desc.d_id == new_desc->d_id) &&
			    (hpdev->desc.ser == new_desc->ser)) {
				hpdev->reported_missing = false;
				found = true;
			}
		}
		spin_unlock_irqrestore(&hbus->device_list_lock, flags);

		if (!found) {
			hpdev = new_pcichild_device(hbus, new_desc);
			if (!hpdev)
				pr_err("couldn't record a child device.\n");
		}
	}

	/* Move missing children to a list on the stack. */
	spin_lock_irqsave(&hbus->device_list_lock, flags);
	do {
		found = false;
		list_for_each(iter, &hbus->children) {
			hpdev = container_of(iter, struct hv_pci_dev,
					     list_entry);
			if (hpdev->reported_missing) {
				found = true;
				hv_pcichild_dec(hpdev, hv_pcidev_ref_childlist);
				list_del(&hpdev->list_entry);
				list_add_tail(&hpdev->list_entry, &removed);
				break;
			}
		}
	} while (found);
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);

	/* Delete everything that should no longer exist. */
	while (!list_empty(&removed)) {
		hpdev = list_first_entry(&removed, struct hv_pci_dev,
					 list_entry);
		list_del(&hpdev->list_entry);
		hv_pcichild_dec(hpdev, hv_pcidev_ref_initial);
	}

	/* Tell the core to rescan bus because there may have been changes. */
	if (hbus->state == hv_pcibus_installed) {
		pci_lock_rescan_remove();
		pci_scan_child_bus(hbus->pci_bus);
		pci_unlock_rescan_remove();
	} else {
		survey_child_resources(hbus);
	}

	up(&hbus->enum_sem);
	hv_pcibus_dec(hbus);
	kfree(dr);
}

/**
 * hv_pci_devices_present() - Handles list of new children
 * @hbus:	Root PCI bus, as understood by this driver
 * @relations:	Packet from host listing children
 *
 * This function is invoked whenever a new list of devices for
 * this bus appears.
 */
static void hv_pci_devices_present(struct hv_pcibus_device *hbus,
				   struct pci_bus_relations *relations)
{
	struct hv_dr_state *dr;
	struct hv_dr_work *dr_wrk;
	unsigned long flags;

	dr_wrk = kzalloc(sizeof(*dr_wrk), GFP_NOWAIT);

	if (!dr_wrk)
		return;

	dr = kzalloc(offsetof(struct hv_dr_state, func) +
		     (sizeof(struct pci_function_description) *
		      (relations->device_count)), GFP_NOWAIT);

	if (!dr)  {
		kfree(dr_wrk);
		return;
	}

	INIT_WORK(&dr_wrk->wrk, pci_devices_present_work);
	dr_wrk->bus = hbus;
	dr->device_count = relations->device_count;
	if (dr->device_count != 0) {
		memcpy(dr->func, relations->func,
		       sizeof(struct pci_function_description) *
		       dr->device_count);
	}

	spin_lock_irqsave(&hbus->device_list_lock, flags);
	list_add_tail(&dr->list_entry, &hbus->dr_list);
	spin_unlock_irqrestore(&hbus->device_list_lock, flags);

	hv_pcibus_inc(hbus);
	schedule_work(&dr_wrk->wrk);
}

/**
 * hv_eject_device_work() - Asynchronously handles ejection
 * @work:	Work struct embedded in internal device struct
 *
 * This function handles ejecting a device.  Windows will
 * attempt to gracefully eject a device, waiting 60 seconds to
 * hear back from the guest OS that this completed successfully.
 * If this timer expires, the device will be forcibly removed.
 */
static void hv_eject_device_work(struct work_struct *work)
{
	struct pci_eject_response *ejct_pkt;
	struct hv_pci_dev *hpdev;
	struct pci_dev *pdev;
	unsigned long flags;
	int wslot;
	struct {
		struct pci_packet pkt;
		u8 buffer[sizeof(struct pci_eject_response) -
			  sizeof(struct pci_message)];
	} ctxt;

	hpdev = container_of(work, struct hv_pci_dev, wrk);

	if (hpdev->state != hv_pcichild_ejecting) {
		hv_pcichild_dec(hpdev, hv_pcidev_ref_pnp);
		return;
	}

	wslot = wslot_to_devfn(hpdev->desc.win_slot.slot);
	pdev = pci_get_domain_bus_and_slot(hpdev->hbus->sysdata.domain, 0,
					   wslot);
	if (pdev) {
		pci_dev_get(pdev);
		pci_stop_and_remove_bus_device(pdev);
		pci_dev_put(pdev);
	}

	memset(&ctxt, 0, sizeof(ctxt));
	ejct_pkt = (struct pci_eject_response *)&ctxt.pkt.message;
	ejct_pkt->message_type = PCI_EJECTION_COMPLETE;
	ejct_pkt->wslot.slot = hpdev->desc.win_slot.slot;
	vmbus_sendpacket(hpdev->hbus->hdev->channel, ejct_pkt,
			 sizeof(*ejct_pkt), (unsigned long)&ctxt.pkt,
			 VM_PKT_DATA_INBAND, 0);

	spin_lock_irqsave(&hpdev->hbus->device_list_lock, flags);
	list_del(&hpdev->list_entry);
	spin_unlock_irqrestore(&hpdev->hbus->device_list_lock, flags);

	hv_pcichild_dec(hpdev, hv_pcidev_ref_childlist);
	hv_pcichild_dec(hpdev, hv_pcidev_ref_pnp);
	hv_pcibus_dec(hpdev->hbus);
}

/**
 * hv_pci_eject_device() - Handles device ejection
 * @hpdev:	Internal device tracking struct
 *
 * This function is invoked when an ejection packet arrives.  It
 * just schedules work so that we don't re-enter the packet
 * delivery code handling the ejection.
 */
static void hv_pci_eject_device(struct hv_pci_dev *hpdev)
{
	hpdev->state = hv_pcichild_ejecting;
	hv_pcichild_inc(hpdev, hv_pcidev_ref_pnp);
	INIT_WORK(&hpdev->wrk, hv_eject_device_work);
	hv_pcibus_inc(hpdev->hbus);
	schedule_work(&hpdev->wrk);
}

/**
 * hv_pci_onchannelcallback() - Handles incoming packets
 * @context:	Internal bus tracking struct
 *
 * This function is invoked whenever the host sends a packet to
 * this channel (which is private to this root PCI bus).
 */
static void hv_pci_onchannelcallback(void *context)
{
	const int packet_size = 0x100;
	int ret;
	struct hv_pcibus_device *hbus = context;
	u32 bytes_recvd;
	u64 req_id;
	struct vmpacket_descriptor *desc;
	unsigned char *buffer;
	int bufferlen = packet_size;
	struct pci_packet *comp_packet;
	struct pci_response *response;
	struct pci_incoming_message *new_message;
	struct pci_bus_relations *bus_rel;
	struct pci_dev_incoming *dev_message;
	struct hv_pci_dev *hpdev;

	buffer = kmalloc(bufferlen, GFP_ATOMIC);
	if (!buffer)
		return;

	while (1) {
	ret = vmbus_recvpacket_raw(hbus->hdev->channel, buffer, bufferlen,
				   &bytes_recvd, &req_id);

		if (ret == -ENOBUFS) {
			kfree(buffer);
			/* Handle large packet */
			bufferlen = bytes_recvd;
			buffer = kmalloc(bytes_recvd, GFP_ATOMIC);
			if (!buffer)
				return;
			continue;
		}

		/*
		 * All incoming packets must be at least as large as a
		 * response.
		 */
		if (bytes_recvd <= sizeof(struct pci_response)) {
			kfree(buffer);
			return;
		}
		desc = (struct vmpacket_descriptor *)buffer;

		switch (desc->type) {
		case VM_PKT_COMP:

			/*
			 * The host is trusted, and thus its safe to interpret
			 * this transaction ID as a pointer.
			 */
			comp_packet = (struct pci_packet *)req_id;
			response = (struct pci_response *)buffer;
			comp_packet->completion_func(comp_packet->compl_ctxt,
						     response,
						     bytes_recvd);
			kfree(buffer);
			return;

		case VM_PKT_DATA_INBAND:

			new_message = (struct pci_incoming_message *)buffer;
			switch (new_message->message_type.message_type) {
			case PCI_BUS_RELATIONS:

				bus_rel = (struct pci_bus_relations *)buffer;
				if (bytes_recvd <
				    offsetof(struct pci_bus_relations, func) +
				    (sizeof(struct pci_function_description) *
				     (bus_rel->device_count))) {
					pr_err("bus relations too small\n");
					break;
				}

				hv_pci_devices_present(hbus, bus_rel);
				break;

			case PCI_EJECT:

				dev_message = (struct pci_dev_incoming *)buffer;
				hpdev = lookup_hv_dev(hbus,
						      dev_message->wslot.slot);
				if (hpdev) {
					hv_pci_eject_device(hpdev);
					hv_pcichild_dec(hpdev,
							hv_pcidev_ref_by_slot);
				}
				break;

			default:
				pr_warn("Unimplemented protocol message %x\n",
					new_message->message_type.message_type);
				break;
			}
			break;

		default:
			pr_err("unhandled packet type %d, tid %llx len %d\n",
			       desc->type, req_id, bytes_recvd);
			break;
		}
		break;
	}
}

/**
 * hv_pci_protocol_negotiation() - Set up protocol
 * @hdev:	Vmbus's tracking struct for this root PCI bus
 *
 * This driver is intended to support running on Windows 10
 * (server) and later versions. It will not run on earlier
 * versions, as they assume that many of the operations which
 * Linux needs accomplished with a spinlock held were done via
 * asynchronous messaging via VMBus.  Windows 10 increases the
 * surface area of PCI emulation so that these actions can take
 * place by suspending a virtual processor for their duration.
 *
 * This function negotiates the channel protocol version,
 * failing if the host doesn't support the necessary protocol
 * level.
 */
static int hv_pci_protocol_negotiation(struct hv_device *hdev)
{
	struct pci_version_request *version_req;
	struct hv_pci_compl comp_pkt;
	struct pci_packet *pkt;
	int ret;

	/*
	 * Initiate the hand shake with the host and negotiate
	 * a version that the host can support. We start with the
	 * highest version number and go down if the host cannot
	 * support it.
	 */

	pkt = kzalloc(sizeof(*pkt) + sizeof(*version_req), GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;

	init_completion(&comp_pkt.host_event);
	pkt->completion_func = hv_pci_generic_compl;
	pkt->compl_ctxt = &comp_pkt;
	version_req = (struct pci_version_request *)&pkt->message;
	version_req->message_type.message_type = PCI_QUERY_PROTOCOL_VERSION;
	version_req->protocol_version = PCI_PROTOCOL_VERSION_CURRENT;

	ret = vmbus_sendpacket(hdev->channel, version_req,
			       sizeof(struct pci_version_request),
			       (unsigned long)pkt, VM_PKT_DATA_INBAND,
			       VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (ret)
		goto exit;

	wait_for_completion(&comp_pkt.host_event);

	if (comp_pkt.completion_status < 0) {
		pr_err("PCI Pass-through VSP failed version request %x\n",
		       comp_pkt.completion_status);
		ret = -EPROTO;
		goto exit;
	}

	ret = 0;

exit:

	kfree(pkt);
	return ret;
}

/**
 * hv_pci_free_bridge_windows() - Release memory regions for the
 * bus
 * @hbus:	Root PCI bus, as understood by this driver
 */
static void hv_pci_free_bridge_windows(struct hv_pcibus_device *hbus)
{
	if (hbus->low_mmio_space && hbus->low_mmio_res) {
		hbus->low_mmio_res->flags |= IORESOURCE_BUSY;
		release_mem_region(hbus->low_mmio_res->start,
				   resource_size(hbus->low_mmio_res));
	}

	if (hbus->high_mmio_space && hbus->high_mmio_res) {
		hbus->high_mmio_res->flags |= IORESOURCE_BUSY;
		release_mem_region(hbus->high_mmio_res->start,
				   resource_size(hbus->high_mmio_res));
	}
}

/**
 * hv_pci_allocate_bridge_windows() - Allocate memory regions
 * for the bus
 * @hbus:	Root PCI bus, as understood by this driver
 *
 * Return: 0 on success, -errno on failure
 */
static int hv_pci_allocate_bridge_windows(struct hv_pcibus_device *hbus)
{
	resource_size_t align;
	int ret;

	if (hbus->low_mmio_space) {
		align = 1ULL << (63 - __builtin_clzll(hbus->low_mmio_space));
		ret = vmbus_allocate_mmio(&hbus->low_mmio_res, hbus->hdev, 0,
					  (u64)(u32)0xffffffff,
					  hbus->low_mmio_space,
					  align, false);
		if (ret) {
			pr_err("Need %llx of low MMIO space. "
				"Consider reconfiguring the VM.\n",
				hbus->low_mmio_space);
			return ret;
		}

		hbus->low_mmio_res->flags |= IORESOURCE_WINDOW;
		hbus->low_mmio_res->flags &= ~IORESOURCE_BUSY;
		pci_add_resource(&hbus->resources_for_children,
				 hbus->low_mmio_res);
	}

	if (hbus->high_mmio_space) {
		align = 1ULL << (63 - __builtin_clzll(hbus->high_mmio_space));
		ret = vmbus_allocate_mmio(&hbus->high_mmio_res, hbus->hdev,
					  0x100000000, -1,
					  hbus->high_mmio_space, align,
					  false);
		if (ret) {
			pr_err("Need %llx of high MMIO space. "
				"Consider reconfiguring the VM.\n",
				hbus->high_mmio_space);
			goto release_low_mmio;
		}

		hbus->high_mmio_res->flags |= IORESOURCE_WINDOW;
		hbus->high_mmio_res->flags &= ~IORESOURCE_BUSY;
		pci_add_resource(&hbus->resources_for_children,
				 hbus->high_mmio_res);
	}

	return 0;

release_low_mmio:

	if (hbus->low_mmio_res) {
		release_mem_region(hbus->low_mmio_res->start,
				   resource_size(hbus->low_mmio_res));
	}

	return ret;
}

/**
 * hv_pci_enter_d0() - Bring the "bus" into the D0 power state
 * @hdev:	Vmbus's tracking struct for this root PCI bus
 *
 * Return: 0 on success, -errno on failure
 */
static int hv_pci_enter_d0(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	struct pci_bus_d0_entry *d0_entry;
	struct hv_pci_compl comp_pkt;
	struct pci_packet *pkt;
	int ret;

	/*
	 * Tell the host that the bus is ready to use, and moved into the
	 * powered-on state.  This includes telling the host which region
	 * of memory-mapped I/O space has been chosen for configuration space
	 * access.
	 */

	pkt = kzalloc(sizeof(*pkt) + sizeof(*d0_entry), GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;

	init_completion(&comp_pkt.host_event);
	pkt->completion_func = hv_pci_generic_compl;
	pkt->compl_ctxt = &comp_pkt;
	d0_entry = (struct pci_bus_d0_entry *)&pkt->message;
	d0_entry->message_type.message_type = PCI_BUS_D0ENTRY;
	d0_entry->mmio_base = hbus->mem_config->start;

	ret = vmbus_sendpacket(hdev->channel, d0_entry, sizeof(*d0_entry),
			       (unsigned long)pkt, VM_PKT_DATA_INBAND,
			       VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (ret)
		goto exit;
	wait_for_completion(&comp_pkt.host_event);

	if (comp_pkt.completion_status < 0) {
		pr_err("PCI Pass-through VSP failed D0 Entry with status %x\n",
		       comp_pkt.completion_status);
		ret = -EPROTO;
		goto exit;
	}

	ret = 0;

exit:

	kfree(pkt);
	return ret;
}

/**
 * hv_pci_query_relations() - Ask host to send list of child
 * devices
 * @hdev:	Vmbus's tracking struct for this root PCI bus
 *
 * Return: 0 on success, -errno on failure
 */
static int hv_pci_query_relations(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	struct pci_message message;
	struct completion comp;
	int ret;

	/*
	 * Ask the host to send along the list of child devices.
	 */

	init_completion(&comp);
	if (cmpxchg(&hbus->survey_event, NULL, &comp))
		return -ENOTEMPTY;

	memset(&message, 0, sizeof(message));
	message.message_type = PCI_QUERY_BUS_RELATIONS;

	ret = vmbus_sendpacket(hdev->channel, &message, sizeof(message),
			       0, VM_PKT_DATA_INBAND, 0);
	if (ret)
		return ret;

	wait_for_completion(&comp);
	return 0;
}

/**
 * hv_send_resources_allocated() - Report local resource choices
 * @hdev:	Vmbus's tracking struct for this root PCI bus
 *
 * The host OS is expecting to be sent a request as a message
 * which contains all the resources that the device will use.
 * The response contains those same resources, "translated"
 * which is to say, the values which should be used by the
 * hardware, when it delivers an interrupt.  (MMIO resources are
 * used in local terms.)  This is nice for Windows, and lines up
 * with the FDO/PDO split, which doesn't exist in Linux.  Linux
 * is deeply expecting to scan an emulated PCI configuration
 * space.  So this message is sent here only to drive the state
 * machine on the host forward.
 *
 * Return: 0 on success, -errno on failure
 */
static int hv_send_resources_allocated(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	struct pci_resources_assigned *res_assigned;
	struct hv_pci_compl comp_pkt;
	struct hv_pci_dev *hpdev;
	struct pci_packet *pkt;
	u32 wslot;
	int ret;

	pkt = kmalloc(sizeof(*pkt) + sizeof(*res_assigned), GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;

	ret = 0;

	for (wslot = 0; wslot < 256; wslot++) {
		hpdev = lookup_hv_dev(hbus, wslot);
		if (!hpdev)
			continue;

		memset(pkt, 0, sizeof(*pkt) + sizeof(*res_assigned));
		init_completion(&comp_pkt.host_event);
		pkt->completion_func = hv_pci_generic_compl;
		pkt->compl_ctxt = &comp_pkt;
		pkt->message.message_type = PCI_RESOURCES_ASSIGNED;
		res_assigned = (struct pci_resources_assigned *)&pkt->message;
		res_assigned->wslot.slot = hpdev->desc.win_slot.slot;

		hv_pcichild_dec(hpdev, hv_pcidev_ref_by_slot);

		ret = vmbus_sendpacket(
			hdev->channel, &pkt->message,
			sizeof(*res_assigned),
			(unsigned long)pkt,
			VM_PKT_DATA_INBAND,
			VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
		if (ret)
			break;

		wait_for_completion(&comp_pkt.host_event);

		if (comp_pkt.completion_status < 0) {
			ret = -EPROTO;
			pr_err("resource allocated returned 0x%x",
			       comp_pkt.completion_status);
			break;
		}
	}

	kfree(pkt);
	return ret;
}

/**
 * hv_send_resources_released() - Report local resources
 * released
 * @hdev:	Vmbus's tracking struct for this root PCI bus
 *
 * Return: 0 on success, -errno on failure
 */
static int hv_send_resources_released(struct hv_device *hdev)
{
	struct hv_pcibus_device *hbus = hv_get_drvdata(hdev);
	struct pci_child_message pkt;
	struct hv_pci_dev *hpdev;
	u32 wslot;
	int ret;

	for (wslot = 0; wslot < 256; wslot++) {
		hpdev = lookup_hv_dev(hbus, wslot);
		if (!hpdev)
			continue;

		memset(&pkt, 0, sizeof(pkt));
		pkt.message_type = PCI_RESOURCES_RELEASED;
		pkt.wslot.slot = hpdev->desc.win_slot.slot;

		hv_pcichild_dec(hpdev, hv_pcidev_ref_by_slot);

		ret = vmbus_sendpacket(hdev->channel, &pkt, sizeof(pkt), 0,
				       VM_PKT_DATA_INBAND, 0);
		if (ret)
			return ret;
	}

	return 0;
}

static void hv_pcibus_inc(struct hv_pcibus_device *hbus)
{
	pr_devel("+> %d %p\n", atomic_read(&hbus->remove_lock) + 1, hbus);
	atomic_inc(&hbus->remove_lock);
}

static void hv_pcibus_dec(struct hv_pcibus_device *hbus)
{
	pr_devel("-> %d %p\n", atomic_read(&hbus->remove_lock) - 1, hbus);
	if (atomic_dec_and_test(&hbus->remove_lock))
		complete(&hbus->remove_event);
}

/**
 * hv_pci_probe() - New VMBus channel probe, for a root PCI bus
 * @hdev:	Vmbus's tracking struct for this root PCI bus
 * @dev_id:	Indentifies the device itself
  *
 * Return: 0 on success, -errno on failure
 */
static int hv_pci_probe(struct hv_device *hdev,
			const struct hv_vmbus_device_id *dev_id)
{
	struct hv_pcibus_device *hbus;
	int ret;

	hbus = kzalloc(sizeof(*hbus), GFP_KERNEL);
	if (!hbus)
		return -ENOMEM;

	pr_devel("New virtual PCIe bus: %p\n", hbus);

	hbus->hdev = hdev;
	atomic_inc(&hbus->remove_lock);
	INIT_LIST_HEAD(&hbus->children);
	INIT_LIST_HEAD(&hbus->dr_list);
	INIT_LIST_HEAD(&hbus->resources_for_children);
	spin_lock_init(&hbus->config_lock);
	spin_lock_init(&hbus->device_list_lock);
	sema_init(&hbus->enum_sem, 1);
	init_completion(&hbus->remove_event);

	/*
	 * The PCI bus "domain" is what is called "segment" in the
	 * ACPI and other specs.  Pull it from the instance ID,
	 * to get something unique.  Bytes 8 and 9 are what is used
	 * in Windows guests, so do the same thing for consistency.
	 */

	hbus->sysdata.domain = hdev->dev_instance.b[9] |
			       hdev->dev_instance.b[8] << 8;

	ret = vmbus_open(hdev->channel, pci_ring_size, pci_ring_size, NULL, 0,
			 hv_pci_onchannelcallback, hbus);
	if (ret)
		goto free_bus;

	hv_set_drvdata(hdev, hbus);

	ret = hv_pci_protocol_negotiation(hdev);
	if (ret)
		goto close;

	/*
	 * Set up a region of MMIO space to use for accessing configuration
	 * space.
	 */
	ret = vmbus_allocate_mmio(&hbus->mem_config, hdev, 0, -1,
				  PCI_CONFIG_MMIO_LENGTH, 0x1000, false);
	if (ret)
		goto close;

	hbus->mem_config->flags |= IORESOURCE_BUSY;
	hbus->cfg_addr = ioremap(hbus->mem_config->start,
				 PCI_CONFIG_MMIO_LENGTH);
	if (!hbus->cfg_addr) {
		pr_err("Unable to map a virtual address for config space\n");
		ret = -ENOMEM;
		goto release;
	}

	hbus->sysdata.fwnode = irq_domain_alloc_fwnode(hbus);
	if (!hbus->sysdata.fwnode) {
		ret = -ENOMEM;
		goto unmap;
	}

	ret = hv_pcie_init_irq_domain(hbus);
	if (ret)
		goto free_fwnode;

	ret = hv_pci_query_relations(hdev);
	if (ret)
		goto free_irq_domain;

	ret = hv_pci_enter_d0(hdev);
	if (ret)
		goto free_irq_domain;

	ret = hv_pci_allocate_bridge_windows(hbus);
	if (ret)
		goto free_irq_domain;

	ret = hv_send_resources_allocated(hdev);
	if (ret)
		goto free_windows;

	prepopulate_bars(hbus);

	hbus->state = hv_pcibus_probed;

	ret = create_root_hv_pci_bus(hbus);
	if (ret)
		goto free_windows;

	return 0;

free_windows:
	hv_pci_free_bridge_windows(hbus);
free_irq_domain:
	irq_domain_remove(hbus->irq_domain);
free_fwnode:
	irq_domain_free_fwnode(hbus->sysdata.fwnode);
unmap:
	iounmap(hbus->cfg_addr);
release:
	release_mem_region(hbus->mem_config->start, PCI_CONFIG_MMIO_LENGTH);
close:
	vmbus_close(hdev->channel);
free_bus:
	kfree(hbus);
	return ret;
}

/**
 * hv_pci_remove() - Remove routine for this VMBus channel
 * @hdev:	Vmbus's tracking struct for this root PCI bus
 *
 * Return: 0 on success, -errno on failure
 */
static int hv_pci_remove(struct hv_device *hdev)
{
	int ret;
	struct hv_pcibus_device *hbus;
	union {
		struct pci_packet teardown_packet;
		__u8 buffer[0x100];
	} pkt;
	struct pci_bus_relations relations;
	struct hv_pci_compl comp_pkt;

	hbus = hv_get_drvdata(hdev);

	ret = hv_send_resources_released(hdev);
	if (ret)
		pr_err("Couldn't send resources released packet(s)\n");

	memset(&pkt.teardown_packet, 0, sizeof(pkt.teardown_packet));
	init_completion(&comp_pkt.host_event);
	pkt.teardown_packet.completion_func = hv_pci_generic_compl;
	pkt.teardown_packet.compl_ctxt = &comp_pkt;
	pkt.teardown_packet.message.message_type = PCI_BUS_D0EXIT;

	ret = vmbus_sendpacket(hdev->channel, &pkt.teardown_packet.message,
			       sizeof(struct pci_message),
			       (unsigned long)&pkt.teardown_packet,
			       VM_PKT_DATA_INBAND,
			       VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (!ret)
		wait_for_completion_timeout(&comp_pkt.host_event, 10 * HZ);

	if (hbus->state == hv_pcibus_installed) {
		/* Remove the bus from PCI's point of view. */
		pci_lock_rescan_remove();
		pci_stop_root_bus(hbus->pci_bus);
		pci_remove_root_bus(hbus->pci_bus);
		pci_unlock_rescan_remove();
	}

	vmbus_close(hdev->channel);

	/* Delete any children which might still exist. */
	memset(&relations, 0, sizeof(relations));
	hv_pci_devices_present(hbus, &relations);

	iounmap(hbus->cfg_addr);
	release_mem_region(hbus->mem_config->start, PCI_CONFIG_MMIO_LENGTH);
	pci_free_resource_list(&hbus->resources_for_children);
	hv_pci_free_bridge_windows(hbus);
	irq_domain_remove(hbus->irq_domain);
	irq_domain_free_fwnode(hbus->sysdata.fwnode);
	hv_pcibus_dec(hbus);
	wait_for_completion(&hbus->remove_event);
	kfree(hbus);
	return 0;
}

static const struct hv_vmbus_device_id hv_pci_id_table[] = {
	/* PCI Pass-through Class ID */
	/* 44C4F61D-4444-4400-9D52-802E27EDE19F */
	{ HV_PCIE_GUID, },
	{ },
};

MODULE_DEVICE_TABLE(vmbus, hv_pci_id_table);

static  struct hv_driver hv_pci_drv = {
	.name		= "hv_pci",
	.id_table	= hv_pci_id_table,
	.probe		= hv_pci_probe,
	.remove		= hv_pci_remove,
};

static void __exit exit_hv_pci_drv(void)
{
	vmbus_driver_unregister(&hv_pci_drv);
}

static int __init init_hv_pci_drv(void)
{
	return vmbus_driver_register(&hv_pci_drv);
}

module_init(init_hv_pci_drv);
module_exit(exit_hv_pci_drv);

MODULE_DESCRIPTION("Hyper-V PCI");
MODULE_LICENSE("GPL v2");
