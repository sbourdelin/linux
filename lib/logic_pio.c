/*
 * Copyright (C) 2017 Hisilicon Limited, All Rights Reserved.
 * Author: Zhichang Yuan <yuanzhichang@hisilicon.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/of.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/rculist.h>
#include <linux/sizes.h>
#include <linux/slab.h>

/* The unique hardware address list. */
static LIST_HEAD(io_range_list);
static DEFINE_MUTEX(io_range_mutex);

/*
 * These are the lists for PIO. The highest PIO_SECT_BITS of PIO is the index.
 */
static struct logic_pio_root logic_pio_root_list[PIO_MAX_SECT] = {
#ifdef CONFIG_INDIRECT_PIO
	/*
	 * At this moment, assign all the other logic PIO space to MMIO.
	 * If more elements added, please adjust the ending index and .sec_max;
	 * Please keep MMIO element started from index ZERO.
	 */
	[PIO_CPU_MMIO ... PIO_INDIRECT - 1] = {
		.sec_head = LIST_HEAD_INIT(logic_pio_root_list[PIO_CPU_MMIO].sec_head),
		.sec_min = PIO_SECT_MIN(PIO_CPU_MMIO),
		.sec_max = PIO_SECT_MAX(PIO_INDIRECT - 1),
	},

	/* The last element */
	[PIO_INDIRECT] = {
		.sec_head = LIST_HEAD_INIT(logic_pio_root_list[PIO_INDIRECT].sec_head),
		.sec_min = PIO_SECT_MIN(PIO_INDIRECT),
		.sec_max = PIO_SECT_MAX(PIO_INDIRECT),
	},
#else
	[PIO_CPU_MMIO] = {
		.sec_head = LIST_HEAD_INIT(logic_pio_root_list[PIO_CPU_MMIO].sec_head),
		.sec_min = PIO_SECT_MIN(PIO_CPU_MMIO),
		.sec_max = PIO_SECT_MAX(PIO_CPU_MMIO),
	},

#endif
};

/*
 * Search a io_range registered which match the fwnode and addr.
 *
 * @fwnode: the host fwnode which must be valid;
 * @start: the start hardware address of this search;
 * @end: the end hardware address of this search. can be equal to @start;
 *
 * return NULL when there is no matched node; IS_ERR() means ERROR;
 * valid virtual address represent a matched node was found.
 */
static struct logic_pio_hwaddr *
logic_pio_find_range_byaddr(struct fwnode_handle *fwnode,
			resource_size_t start, resource_size_t end)
{
	struct logic_pio_hwaddr *range;

	list_for_each_entry_rcu(range, &io_range_list, list) {
		if (!range->pio_peer) {
			pr_warn("Invalid cpu addr node(%pa) in list!\n",
				&range->hw_start);
			continue;
		}
		if (range->fwnode != fwnode)
			continue;
		/* without any overlap with current range */
		if (start >= range->hw_start + range->size ||
			end < range->hw_start)
			continue;
		/* overlap is not supported now. */
		if (start < range->hw_start ||
			end >= range->hw_start + range->size)
			return ERR_PTR(-EBUSY);
		/* had been registered. */
		return range;
	}

	return NULL;
}


static int logic_pio_alloc_range(struct logic_pio_root *root,
		resource_size_t size, unsigned long align,
		struct list_head **prev, resource_size_t *pio_alloc)
{
	struct logic_pio_sect *entry;
	resource_size_t tmp_start;
	resource_size_t idle_start, idle_end;

	idle_start = root->sec_min;
	*prev = &root->sec_head;
	list_for_each_entry_rcu(entry, &root->sec_head, list) {
		if (!entry->hwpeer ||
			idle_start > entry->io_start) {
			WARN(1, "skip an invalid io range during traversal!\n");
			goto nextentry;
		}
		/* set the end edge. */
		if (idle_start == entry->io_start) {
			struct logic_pio_sect *next;

			idle_start = entry->io_start + entry->hwpeer->size;
			next = list_next_or_null_rcu(&root->sec_head,
				&entry->list, struct logic_pio_sect, list);
			if (next) {
				entry = next;
			} else {
				*prev = &entry->list;
				break;
			}
		}
		idle_end = entry->io_start - 1;

		/* contiguous range... */
		if (idle_start > idle_end)
			goto nextentry;

		tmp_start = idle_start;
		idle_start = ALIGN(idle_start, align);
		if (idle_start >= tmp_start &&
			idle_start + size <= idle_end) {
			*prev = &entry->list;
			*pio_alloc = idle_start;
			return 0;
		}

nextentry:
		idle_start = entry->io_start + entry->hwpeer->size;
		*prev = &entry->list;
	}
	/* check the last free gap... */
	idle_end = root->sec_max;

	tmp_start = idle_start;
	idle_start = ALIGN(idle_start, align);
	if (idle_start >= tmp_start &&
		idle_start + size <= idle_end) {
		*pio_alloc = idle_start;
		return 0;
	}

	return -EBUSY;
}

/*
 * register a io range node in the io range list.
 *
 * @newrange: pointer to the io range to be registered.
 *
 * return 'newrange' when success, ERR_VALUE() is for failures.
 * specially, return a valid pointer which is not equal to 'newrange' when
 * the io range had been registered before.
 */
struct logic_pio_hwaddr
*logic_pio_register_range(struct logic_pio_hwaddr *newrange,
		unsigned long align)
{
	struct logic_pio_hwaddr *range;
	struct logic_pio_sect *newsect;
	resource_size_t pio_alloc;
	struct list_head *prev, *hwprev;
	unsigned long sect_id;
	int err;

	if (!newrange || !newrange->fwnode || !newrange->size)
		return ERR_PTR(-EINVAL);

	sect_id = newrange->flags;
	if (sect_id >= PIO_MAX_SECT)
		return ERR_PTR(-EINVAL);

	mutex_lock(&io_range_mutex);
	range = logic_pio_find_range_byaddr(newrange->fwnode,
			newrange->hw_start,
			newrange->hw_start + newrange->size - 1);
	if (range) {
		if (!IS_ERR(range))
			pr_info("the request IO range had been registered!\n");
		else
			pr_err("registering IO[%pa - sz%pa) got failed!\n",
				&newrange->hw_start, &newrange->size);
		mutex_unlock(&io_range_mutex);
		return range;
	}

	err = logic_pio_alloc_range(&logic_pio_root_list[sect_id],
			newrange->size, align, &prev, &pio_alloc);
	if (err) {
		pr_err("can't find free %pa logical IO range!\n",
			&newrange->size);
		goto exitproc;
	}

	if (prev == &logic_pio_root_list[sect_id].sec_head) {
		hwprev = &io_range_list;
	} else {
		newsect = to_pio_sect(prev);
		hwprev = &newsect->hwpeer->list;
	}

	newsect = kzalloc(sizeof(*newsect), GFP_KERNEL);
	if (!newsect) {
		err = -ENOMEM;
		goto exitproc;
	}
	newsect->io_start = pio_alloc;
	newsect->hwpeer = newrange;
	list_add_rcu(&newsect->list, prev);

	newrange->pio_peer = newsect;
	list_add_rcu(&newrange->list, hwprev);

exitproc:
	mutex_unlock(&io_range_mutex);
	return err ? ERR_PTR(err) : newrange;
}

/*
 * traverse the io_range_list to find the registered node whose device node
 * and/or physical IO address match to.
 */
struct logic_pio_hwaddr *find_io_range_by_fwnode(struct fwnode_handle *fwnode)
{
	struct logic_pio_hwaddr *range;

	list_for_each_entry_rcu(range, &io_range_list, list) {
		if (range->fwnode == fwnode)
			return range;
	}
	return NULL;
}

/*
 * Translate the input logical pio to the corresponding hardware address.
 * The input pio should be unique in the whole logical PIO space.
 */
resource_size_t logic_pio_to_hwaddr(unsigned long pio)
{
	struct logic_pio_sect *entry;
	struct logic_pio_root *root;

	/* The caller should check the section id is valid. */
	root = &logic_pio_root_list[PIO_SECT_ID(pio)];
	list_for_each_entry_rcu(entry, &root->sec_head, list) {
		if (!entry->hwpeer) {
			pr_warn("Invalid PIO entry(%pa) in list!\n",
				&entry->io_start);
			continue;
		}
		if (pio < entry->io_start)
			break;

		if (pio < entry->io_start + entry->hwpeer->size)
			return pio - entry->io_start + entry->hwpeer->hw_start;
	}

	return -1;
}

/*
 * This function is generic for translating a hardware address to logical PIO.
 * @hw_addr: the hardware address of host, can be CPU address or host-local
 *		address;
 */
unsigned long
logic_pio_trans_hwaddr(struct fwnode_handle *fwnode, resource_size_t addr)
{
	struct logic_pio_hwaddr *range;

	range = logic_pio_find_range_byaddr(fwnode, addr, addr);
	if (!range)
		return -1;

	return addr - range->hw_start + range->pio_peer->io_start;
}

unsigned long
logic_pio_trans_cpuaddr(resource_size_t addr)
{
	struct logic_pio_hwaddr *range;

	list_for_each_entry_rcu(range, &io_range_list, list) {
		if (!range->pio_peer) {
			pr_warn("Invalid cpu addr node(%pa) in list!\n",
				&range->hw_start);
			continue;
		}
		if (range->flags != PIO_CPU_MMIO)
			continue;
		if (addr >= range->hw_start &&
			addr < range->hw_start + range->size)
			return addr - range->hw_start +
				range->pio_peer->io_start;
	}
	return -1;
}

#if defined(CONFIG_INDIRECT_PIO) && defined(PCI_IOBASE)
static struct logic_pio_hwaddr *find_io_range(unsigned long pio)
{
	struct logic_pio_sect *entry;
	struct logic_pio_root *root;

	root = &logic_pio_root_list[PIO_SECT_ID(pio)];
	if (pio < root->sec_min || pio > root->sec_max)
		return NULL;
	/*
	 * non indirectIO section, no need to convert the addr. Jump to mmio ops
	 * directly.
	 */
	if (&root->sec_head == &logic_pio_root_list[PIO_CPU_MMIO].sec_head)
		return NULL;
	list_for_each_entry_rcu(entry, &root->sec_head, list) {
		if (!entry->hwpeer) {
			pr_warn("Invalid PIO entry(%pa) in list!\n",
				&entry->io_start);
			continue;
		}
		if (pio < entry->io_start)
			break;

		if (pio < entry->io_start + entry->hwpeer->size)
			return entry->hwpeer;
	}

	return NULL;
}

#define BUILD_LOGIC_IO(bw, type)					\
type logic_in##bw(unsigned long addr)					\
{									\
	struct logic_pio_hwaddr *entry = find_io_range(addr);		\
									\
	if (entry && entry->ops)					\
		return entry->ops->pfin(entry->devpara,			\
					addr, sizeof(type));		\
	return read##bw(PCI_IOBASE + addr);				\
}									\
									\
void logic_out##bw(type value, unsigned long addr)			\
{									\
	struct logic_pio_hwaddr *entry = find_io_range(addr);		\
									\
	if (entry && entry->ops)					\
		entry->ops->pfout(entry->devpara,			\
					addr, value, sizeof(type));	\
	else								\
		write##bw(value, PCI_IOBASE + addr);			\
}									\
									\
void logic_ins##bw(unsigned long addr, void *buffer, unsigned int count)\
{									\
	struct logic_pio_hwaddr *entry = find_io_range(addr);		\
									\
	if (entry && entry->ops)					\
		entry->ops->pfins(entry->devpara,			\
				addr, buffer, sizeof(type), count);	\
	else								\
		reads##bw(PCI_IOBASE + addr, buffer, count);		\
}									\
									\
void logic_outs##bw(unsigned long addr, const void *buffer,		\
		    unsigned int count)					\
{									\
	struct logic_pio_hwaddr *entry = find_io_range(addr);		\
									\
	if (entry && entry->ops)					\
		entry->ops->pfouts(entry->devpara,			\
				addr, buffer, sizeof(type), count);	\
	else								\
		writes##bw(PCI_IOBASE + addr, buffer, count);	\
}

BUILD_LOGIC_IO(b, u8)

EXPORT_SYMBOL(logic_inb);
EXPORT_SYMBOL(logic_outb);
EXPORT_SYMBOL(logic_insb);
EXPORT_SYMBOL(logic_outsb);

BUILD_LOGIC_IO(w, u16)

EXPORT_SYMBOL(logic_inw);
EXPORT_SYMBOL(logic_outw);
EXPORT_SYMBOL(logic_insw);
EXPORT_SYMBOL(logic_outsw);

BUILD_LOGIC_IO(l, u32)

EXPORT_SYMBOL(logic_inl);
EXPORT_SYMBOL(logic_outl);
EXPORT_SYMBOL(logic_insl);
EXPORT_SYMBOL(logic_outsl);
#endif /* CONFIG_INDIRECT_PIO && PCI_IOBASE */
