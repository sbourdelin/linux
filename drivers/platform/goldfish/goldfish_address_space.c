// SPDX-License-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>

#include <linux/device.h>
#include <linux/pci_regs.h>
#include <linux/pci_ids.h>
#include <linux/pci.h>

#include <uapi/linux/goldfish/goldfish_address_space.h>

MODULE_DESCRIPTION("A driver for the Goldfish Android emulator that occupies "
		   "address space to use it with the memory sharing device "
		   "on the QEMU side. The memory sharding device allocates "
		   "subranges and populate them with actual RAM. "
		   "This allows sharing host's memory with the guest.");
MODULE_AUTHOR("Roman Kiryanov <rkir@google.com>");
MODULE_LICENSE("GPL v2");

enum as_register_id {
	AS_REGISTER_COMMAND = 0,
	AS_REGISTER_STATUS = 4,
	AS_REGISTER_GUEST_PAGE_SIZE = 8,
	AS_REGISTER_BLOCK_SIZE_LOW = 12,
	AS_REGISTER_BLOCK_SIZE_HIGH = 16,
	AS_REGISTER_BLOCK_OFFSET_LOW = 20,
	AS_REGISTER_BLOCK_OFFSET_HIGH = 24,
};

enum as_command_id {
	AS_COMMAND_ALLOCATE_BLOCK = 1,
	AS_COMMAND_DEALLOCATE_BLOCK = 2,
};

#define AS_PCI_VENDOR_ID	0x607D
#define AS_PCI_DEVICE_ID	0xF153
#define AS_MAGIC_U32		(AS_PCI_VENDOR_ID << 16 | AS_PCI_DEVICE_ID)
#define AS_ALLOCATED_BLOCKS_INITIAL_CAPACITY 32

enum as_pci_bar_id {
	AS_PCI_CONTROL_BAR_ID = 0,
	AS_PCI_AREA_BAR_ID = 1,
};

struct as_driver_state;

struct as_device_state {
	u32	magic;

	struct miscdevice	miscdevice;
	struct pci_dev		*dev;
	struct as_driver_state	*driver_state;

	void __iomem		*io_registers;

	void			*address_area;	/* to claim the address space */

	/* physical address to allocate from */
	unsigned long		address_area_phys_address;

	struct mutex		registers_lock;	/* protects registers */

	wait_queue_head_t	wake_queue;	/* to wait for the hardware */

	int			hw_done;	/* to say hw is done */
};

struct as_block {
	u64 offset;
	u64 size;
};

struct as_allocated_blocks {
	struct as_device_state *state;

	struct as_block *blocks;  /* a dynamic array of allocated blocks */
	int blocks_size;
	int blocks_capacity;
	struct mutex blocks_lock; /* protects operations with blocks */
};

static void __iomem *as_register_address(void __iomem *base,
					 int offset)
{
	return ((char __iomem *)base) + offset;
}

static void as_write_register(void __iomem *registers,
			      int offset,
			      u32 value)
{
	writel(value, as_register_address(registers, offset));
}

static u32 as_read_register(void __iomem *registers, int offset)
{
	return readl(as_register_address(registers, offset));
}

static int
as_talk_to_hardware(struct as_device_state *state, enum as_command_id cmd)
{
	state->hw_done = 0;
	as_write_register(state->io_registers, AS_REGISTER_COMMAND, cmd);
	wait_event(state->wake_queue, state->hw_done);
	return -as_read_register(state->io_registers,
				 AS_REGISTER_STATUS);
}

static long
as_ioctl_allocate_block_locked_impl(struct as_device_state *state,
				    u64 *size, u64 *offset)
{
	long res;

	as_write_register(state->io_registers,
			  AS_REGISTER_BLOCK_SIZE_LOW,
			  lower_32_bits(*size));
	as_write_register(state->io_registers,
			  AS_REGISTER_BLOCK_SIZE_HIGH,
			  upper_32_bits(*size));

	res = as_talk_to_hardware(state, AS_COMMAND_ALLOCATE_BLOCK);
	if (!res) {
		u64 low = as_read_register(state->io_registers,
					   AS_REGISTER_BLOCK_OFFSET_LOW);
		u64 high = as_read_register(state->io_registers,
					    AS_REGISTER_BLOCK_OFFSET_HIGH);
		*offset = low | (high << 32);

		low = as_read_register(state->io_registers,
				       AS_REGISTER_BLOCK_SIZE_LOW);
		high = as_read_register(state->io_registers,
					AS_REGISTER_BLOCK_SIZE_HIGH);
		*size = low | (high << 32);
	}

	return res;
}

static long
as_ioctl_unallocate_block_locked_impl(struct as_device_state *state, u64 offset)
{
	as_write_register(state->io_registers,
			  AS_REGISTER_BLOCK_OFFSET_LOW,
			  lower_32_bits(offset));
	as_write_register(state->io_registers,
			  AS_REGISTER_BLOCK_OFFSET_HIGH,
			  upper_32_bits(offset));

	return as_talk_to_hardware(state, AS_COMMAND_DEALLOCATE_BLOCK);
}

static int as_blocks_grow_capacity(int old_capacity)
{
	return old_capacity + old_capacity;
}

static int
as_blocks_insert(struct as_allocated_blocks *allocated_blocks,
		 u64 offset,
		 u64 size)
{
	int blocks_size;

	if (mutex_lock_interruptible(&allocated_blocks->blocks_lock))
		return -ERESTARTSYS;

	blocks_size = allocated_blocks->blocks_size;

	if (allocated_blocks->blocks_capacity == blocks_size) {
		int new_capacity =
			as_blocks_grow_capacity(
				allocated_blocks->blocks_capacity);
		struct as_block *new_blocks =
			kcalloc(new_capacity,
				sizeof(allocated_blocks->blocks[0]),
				GFP_KERNEL);

		if (!new_blocks) {
			mutex_unlock(&allocated_blocks->blocks_lock);
			return -ENOMEM;
		}

		memcpy(new_blocks, allocated_blocks->blocks,
		       blocks_size * sizeof(allocated_blocks->blocks[0]));

		kfree(allocated_blocks->blocks);
		allocated_blocks->blocks = new_blocks;
		allocated_blocks->blocks_capacity = new_capacity;
	}

	allocated_blocks->blocks[blocks_size] =
		(struct as_block){ .offset = offset, .size = size };
	allocated_blocks->blocks_size = blocks_size + 1;

	mutex_unlock(&allocated_blocks->blocks_lock);
	return 0;
}

static int
as_blocks_remove(struct as_allocated_blocks *allocated_blocks, u64 offset)
{
	long res = -ENXIO;
	struct as_block *blocks;
	int blocks_size;
	int i;

	if (mutex_lock_interruptible(&allocated_blocks->blocks_lock))
		return -ERESTARTSYS;

	blocks = allocated_blocks->blocks;
	blocks_size = allocated_blocks->blocks_size;

	for (i = 0; i < blocks_size; ++i) {
		if (offset == blocks[i].offset) {
			int last = blocks_size - 1;

			if (last > i)
				blocks[i] = blocks[last];

			--allocated_blocks->blocks_size;
			res = 0;
			break;
		}
	}

	mutex_unlock(&allocated_blocks->blocks_lock);
	return res;
}

static int
as_blocks_check_if_mine(struct as_allocated_blocks *allocated_blocks,
			u64 offset,
			u64 size)
{
	const u64 end = offset + size;
	int res = -EPERM;
	struct as_block *block;
	int blocks_size;

	if (mutex_lock_interruptible(&allocated_blocks->blocks_lock))
		return -ERESTARTSYS;

	block = allocated_blocks->blocks;
	blocks_size = allocated_blocks->blocks_size;

	for (; blocks_size > 0; --blocks_size, ++block) {
		u64 block_offset = block->offset;
		u64 block_end = block_offset + block->size;

		if (offset >= block_offset && end <= block_end) {
			res = 0;
			break;
		}
	}

	mutex_unlock(&allocated_blocks->blocks_lock);
	return res;
}

static int as_open(struct inode *inode, struct file *filp)
{
	struct as_allocated_blocks *allocated_blocks;

	allocated_blocks = kzalloc(sizeof(*allocated_blocks), GFP_KERNEL);
	if (!allocated_blocks)
		return -ENOMEM;

	allocated_blocks->state =
		container_of(filp->private_data,
			     struct as_device_state,
			     miscdevice);

	allocated_blocks->blocks =
		kcalloc(AS_ALLOCATED_BLOCKS_INITIAL_CAPACITY,
			sizeof(allocated_blocks->blocks[0]),
			GFP_KERNEL);
	if (!allocated_blocks->blocks) {
		kfree(allocated_blocks);
		return -ENOMEM;
	}

	allocated_blocks->blocks_size = 0;
	allocated_blocks->blocks_capacity =
		AS_ALLOCATED_BLOCKS_INITIAL_CAPACITY;
	mutex_init(&allocated_blocks->blocks_lock);

	filp->private_data = allocated_blocks;
	return 0;
}

static int as_release(struct inode *inode, struct file *filp)
{
	struct as_allocated_blocks *allocated_blocks = filp->private_data;
	struct as_device_state *state;
	int blocks_size;
	int i;

	state = allocated_blocks->state;
	blocks_size = allocated_blocks->blocks_size;

	if (mutex_lock_interruptible(&state->registers_lock))
		return -ERESTARTSYS;

	for (i = 0; i < blocks_size; ++i) {
		as_ioctl_unallocate_block_locked_impl(
			state, allocated_blocks->blocks[i].offset);
	}

	mutex_unlock(&state->registers_lock);

	kfree(allocated_blocks->blocks);
	kfree(allocated_blocks);
	return 0;
}

static int as_mmap_impl(struct as_device_state *state,
			size_t size,
			struct vm_area_struct *vma)
{
	unsigned long pfn = (state->address_area_phys_address >> PAGE_SHIFT) +
		vma->vm_pgoff;

	return remap_pfn_range(vma,
			       vma->vm_start,
			       pfn,
			       size,
			       vma->vm_page_prot);
}

static int as_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct as_allocated_blocks *allocated_blocks = filp->private_data;
	size_t size = PAGE_ALIGN(vma->vm_end - vma->vm_start);
	int res;

	res = as_blocks_check_if_mine(allocated_blocks,
				      vma->vm_pgoff << PAGE_SHIFT,
				      size);

	if (res)
		return res;
	else
		return as_mmap_impl(allocated_blocks->state, size, vma);
}

static long as_ioctl_allocate_block_impl(
	struct as_device_state *state,
	struct goldfish_address_space_allocate_block *request)
{
	long res;

	if (mutex_lock_interruptible(&state->registers_lock))
		return -ERESTARTSYS;

	res = as_ioctl_allocate_block_locked_impl(state,
						  &request->size,
						  &request->offset);
	if (!res) {
		request->phys_addr =
			state->address_area_phys_address + request->offset;
	}

	mutex_unlock(&state->registers_lock);
	return res;
}

static void
as_ioctl_unallocate_block_impl(struct as_device_state *state, u64 offset)
{
	mutex_lock(&state->registers_lock);
	as_ioctl_unallocate_block_locked_impl(state, offset);
	mutex_unlock(&state->registers_lock);
}

static long
as_ioctl_allocate_block(struct as_allocated_blocks *allocated_blocks,
			void __user *ptr)
{
	long res;
	struct as_device_state *state = allocated_blocks->state;
	struct goldfish_address_space_allocate_block request;

	if (copy_from_user(&request, ptr, sizeof(request)))
		return -EFAULT;

	res = as_ioctl_allocate_block_impl(state, &request);
	if (!res) {
		res = as_blocks_insert(allocated_blocks,
				       request.offset,
				       request.size);

		if (res) {
			as_ioctl_unallocate_block_impl(state, request.offset);
		} else if (copy_to_user(ptr, &request, sizeof(request))) {
			as_ioctl_unallocate_block_impl(state, request.offset);
			res = -EFAULT;
		}
	}

	return res;
}

static long
as_ioctl_unallocate_block(struct as_allocated_blocks *allocated_blocks,
			  void __user *ptr)
{
	long res;
	u64 offset;

	if (copy_from_user(&offset, ptr, sizeof(offset)))
		return -EFAULT;

	res = as_blocks_remove(allocated_blocks, offset);
	if (!res)
		as_ioctl_unallocate_block_impl(allocated_blocks->state, offset);

	return res;
}

static long as_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct as_allocated_blocks *allocated_blocks = filp->private_data;

	switch (cmd) {
	case GOLDFISH_ADDRESS_SPACE_IOCTL_ALLOCATE_BLOCK:
		return as_ioctl_allocate_block(allocated_blocks,
					       (void __user *)arg);

	case GOLDFISH_ADDRESS_SPACE_IOCTL_DEALLOCATE_BLOCK:
		return as_ioctl_unallocate_block(allocated_blocks,
						 (void __user *)arg);

	default:
		return -ENOTTY;
	}
}

static const struct file_operations userspace_file_operations = {
	.owner = THIS_MODULE,
	.open = as_open,
	.release = as_release,
	.mmap = as_mmap,
	.unlocked_ioctl = as_ioctl,
	.compat_ioctl = as_ioctl,
};

static void __iomem __must_check *ioremap_pci_bar(struct pci_dev *dev,
						  int bar_id)
{
	void __iomem *io;
	unsigned long size = pci_resource_len(dev, bar_id);

	if (!size)
		return IOMEM_ERR_PTR(-ENXIO);

	io = ioremap(pci_resource_start(dev, bar_id), size);
	if (!io)
		return IOMEM_ERR_PTR(-ENOMEM);

	return io;
}

static void __must_check *memremap_pci_bar(struct pci_dev *dev,
					   int bar_id,
					   unsigned long flags)
{
	void *mem;
	unsigned long size = pci_resource_len(dev, bar_id);

	if (!size)
		return ERR_PTR(-ENXIO);

	mem = memremap(pci_resource_start(dev, bar_id), size, flags);
	if (!mem)
		return ERR_PTR(-ENOMEM);

	return mem;
}

static irqreturn_t __must_check as_interrupt_impl(struct as_device_state *state)
{
	state->hw_done = 1;
	wake_up_interruptible(&state->wake_queue);
	return IRQ_HANDLED;
}

static irqreturn_t as_interrupt(int irq, void *dev_id)
{
	struct as_device_state *state = dev_id;

	return (state->magic == AS_MAGIC_U32)
		? as_interrupt_impl(state) : IRQ_NONE;
}

static void fill_miscdevice(struct miscdevice *miscdev)
{
	memset(miscdev, 0, sizeof(*miscdev));

	miscdev->minor = MISC_DYNAMIC_MINOR;
	miscdev->name = GOLDFISH_ADDRESS_SPACE_DEVICE_NAME;
	miscdev->fops = &userspace_file_operations;
}

static int __must_check
create_as_device(struct pci_dev *dev, const struct pci_device_id *id)
{
	int res;
	struct as_device_state *state;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	res = pci_request_region(dev,
				 AS_PCI_CONTROL_BAR_ID,
				 "Address space control");
	if (res) {
		pr_err("(bn 0x%X, sn 0x%X) failed to allocate PCI resource for BAR%d",
		       dev->bus->number,
		       dev->devfn,
		       AS_PCI_CONTROL_BAR_ID);
		goto out_free_device_state;
	}

	res = pci_request_region(dev,
				 AS_PCI_AREA_BAR_ID,
				 "Address space area");
	if (res) {
		pr_err("(bn 0x%X, sn 0x%X) failed to allocate PCI resource for BAR%d",
		       dev->bus->number,
		       dev->devfn,
		       AS_PCI_AREA_BAR_ID);
		goto out_release_control_bar;
	}

	fill_miscdevice(&state->miscdevice);
	res = misc_register(&state->miscdevice);
	if (res)
		goto out_release_area_bar;

	state->io_registers = ioremap_pci_bar(dev,
					      AS_PCI_CONTROL_BAR_ID);
	if (IS_ERR(state->io_registers)) {
		res = PTR_ERR(state->io_registers);
		goto out_misc_deregister;
	}

	state->address_area = memremap_pci_bar(dev,
					       AS_PCI_AREA_BAR_ID,
					       MEMREMAP_WB);
	if (IS_ERR(state->address_area)) {
		res = PTR_ERR(state->address_area);
		goto out_iounmap;
	}

	state->address_area_phys_address =
		pci_resource_start(dev, AS_PCI_AREA_BAR_ID);

	res = request_irq(dev->irq,
			  as_interrupt, IRQF_SHARED,
			  KBUILD_MODNAME, state);
	if (res)
		goto out_memunmap;

	as_write_register(state->io_registers,
			  AS_REGISTER_GUEST_PAGE_SIZE,
			  PAGE_SIZE);

	state->magic = AS_MAGIC_U32;
	state->dev = dev;
	mutex_init(&state->registers_lock);
	init_waitqueue_head(&state->wake_queue);
	pci_set_drvdata(dev, state);

	return 0;

out_memunmap:
	memunmap(state->address_area);
out_iounmap:
	iounmap(state->io_registers);
out_misc_deregister:
	misc_deregister(&state->miscdevice);
out_release_area_bar:
	pci_release_region(dev, AS_PCI_AREA_BAR_ID);
out_release_control_bar:
	pci_release_region(dev, AS_PCI_CONTROL_BAR_ID);
out_free_device_state:
	kzfree(state);

	return res;
}

static void as_destroy_device(struct as_device_state *state)
{
	free_irq(state->dev->irq, state);
	memunmap(state->address_area);
	iounmap(state->io_registers);
	misc_deregister(&state->miscdevice);
	pci_release_region(state->dev, AS_PCI_AREA_BAR_ID);
	pci_release_region(state->dev, AS_PCI_CONTROL_BAR_ID);
	kfree(state);
}

static int __must_check as_pci_probe(struct pci_dev *dev,
				     const struct pci_device_id *id)
{
	int res;
	u8 hardware_revision;

	res = pci_enable_device(dev);
	if (res)
		return res;

	res = pci_read_config_byte(dev, PCI_REVISION_ID, &hardware_revision);
	if (res)
		goto out_disable_pci;

	switch (hardware_revision) {
	case 1:
		res = create_as_device(dev, id);
		break;

	default:
		res = -ENODEV;
		goto out_disable_pci;
	}

	return 0;

out_disable_pci:
	pci_disable_device(dev);

	return res;
}

static void as_pci_remove(struct pci_dev *dev)
{
	struct as_device_state *state = pci_get_drvdata(dev);

	as_destroy_device(state);
	pci_disable_device(dev);
}

static const struct pci_device_id as_pci_tbl[] = {
	{ PCI_DEVICE(AS_PCI_VENDOR_ID, AS_PCI_DEVICE_ID), },
	{ }
};
MODULE_DEVICE_TABLE(pci, as_pci_tbl);

static struct pci_driver goldfish_address_space_driver = {
	.name		= GOLDFISH_ADDRESS_SPACE_DEVICE_NAME,
	.id_table	= as_pci_tbl,
	.probe		= as_pci_probe,
	.remove		= as_pci_remove,
};

module_pci_driver(goldfish_address_space_driver);
