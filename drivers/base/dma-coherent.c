/*
 * Coherent per-device memory handling.
 * Borrowed from i386
 */
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>

#ifdef CONFIG_PROC_FS
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#endif

struct dma_coherent_mem {
	void		*virt_base;
	dma_addr_t	device_base;
	unsigned long	pfn_base;
	int		size;
	int		flags;
	unsigned long	*bitmap;
	spinlock_t	spinlock;
	bool		use_dev_dma_pfn_offset;
	int		used;
	int		highwatermark;
	int		errs;
};

#ifdef CONFIG_PROC_FS
struct dmacoherent_region {
	struct list_head list;
	struct device *dev;
};

static LIST_HEAD(dmacoherent_region_list);
static DEFINE_MUTEX(dmacoherent_region_list_lock);

static int dmacoherent_region_add(struct device *dev)
{
	struct dmacoherent_region *rp;

	rp = kzalloc(sizeof(*rp), GFP_KERNEL);
	if (!rp)
		return -ENOMEM;

	rp->dev = dev;

	mutex_lock(&dmacoherent_region_list_lock);
	list_add(&rp->list, &dmacoherent_region_list);
	mutex_unlock(&dmacoherent_region_list_lock);
	dev_info(dev, "Registered DMA-coherent pool with /proc/dmainfo accounting\n");

	return 0;
}

static void dmacoherent_region_del(struct device *dev)
{
	struct dmacoherent_region *rp;

	mutex_lock(&dmacoherent_region_list_lock);
	list_for_each_entry(rp, &dmacoherent_region_list, list) {
		if (rp->dev == dev) {
			list_del(&rp->list);
			kfree(rp);
			break;
		}
	}
	mutex_unlock(&dmacoherent_region_list_lock);
}
#else
static int dmacoherent_region_add(struct device *dev) { return 0; }
static void dmacoherent_region_del(struct device *dev) { return; }
#endif

static struct dma_coherent_mem *dma_coherent_default_memory __ro_after_init;

static inline struct dma_coherent_mem *dev_get_coherent_memory(struct device *dev)
{
	if (dev && dev->dma_mem)
		return dev->dma_mem;
#ifdef CONFIG_DMA_CMA
	if (dev && dev->cma_area)
		return NULL;
#endif
	return dma_coherent_default_memory;
}

static inline dma_addr_t dma_get_device_base(struct device *dev,
					     struct dma_coherent_mem * mem)
{
	if (mem->use_dev_dma_pfn_offset)
		return (mem->pfn_base - dev->dma_pfn_offset) << PAGE_SHIFT;
	else
		return mem->device_base;
}

static bool dma_init_coherent_memory(
	phys_addr_t phys_addr, dma_addr_t device_addr, size_t size, int flags,
	struct dma_coherent_mem **mem)
{
	struct dma_coherent_mem *dma_mem = NULL;
	void __iomem *mem_base = NULL;
	int pages = size >> PAGE_SHIFT;
	int bitmap_size = BITS_TO_LONGS(pages) * sizeof(long);

	if ((flags & (DMA_MEMORY_MAP | DMA_MEMORY_IO)) == 0)
		goto out;
	if (!size)
		goto out;

	if (flags & DMA_MEMORY_MAP)
		mem_base = memremap(phys_addr, size, MEMREMAP_WC);
	else
		mem_base = ioremap(phys_addr, size);
	if (!mem_base)
		goto out;

	dma_mem = kzalloc(sizeof(struct dma_coherent_mem), GFP_KERNEL);
	if (!dma_mem)
		goto out;
	dma_mem->bitmap = kzalloc(bitmap_size, GFP_KERNEL);
	if (!dma_mem->bitmap)
		goto out;

	dma_mem->virt_base = mem_base;
	dma_mem->device_base = device_addr;
	dma_mem->pfn_base = PFN_DOWN(phys_addr);
	dma_mem->size = pages;
	dma_mem->flags = flags;
	spin_lock_init(&dma_mem->spinlock);

	*mem = dma_mem;
	return true;

out:
	kfree(dma_mem);
	if (mem_base) {
		if (flags & DMA_MEMORY_MAP)
			memunmap(mem_base);
		else
			iounmap(mem_base);
	}
	return false;
}

static void dma_release_coherent_memory(struct dma_coherent_mem *mem)
{
	if (!mem)
		return;

	if (mem->flags & DMA_MEMORY_MAP)
		memunmap(mem->virt_base);
	else
		iounmap(mem->virt_base);
	kfree(mem->bitmap);
	kfree(mem);
}

static int dma_assign_coherent_memory(struct device *dev,
				      struct dma_coherent_mem *mem)
{
	if (!dev)
		return -ENODEV;

	if (dev->dma_mem)
		return -EBUSY;

	dev->dma_mem = mem;
	/* FIXME: this routine just ignores DMA_MEMORY_INCLUDES_CHILDREN */

	return 0;
}

int dma_declare_coherent_memory(struct device *dev, phys_addr_t phys_addr,
				dma_addr_t device_addr, size_t size, int flags)
{
	struct dma_coherent_mem *mem;
	int ret;

	if (!dma_init_coherent_memory(phys_addr, device_addr, size, flags,
				      &mem))
		return 0;

	if (dma_assign_coherent_memory(dev, mem) != 0)
		goto errout;

	ret = (flags & DMA_MEMORY_MAP ? DMA_MEMORY_MAP : DMA_MEMORY_IO);

	if (dmacoherent_region_add(dev) == 0)
		return ret;

	dev->dma_mem = NULL;
errout:
	dma_release_coherent_memory(mem);
	return 0;
}
EXPORT_SYMBOL(dma_declare_coherent_memory);

void dma_release_declared_memory(struct device *dev)
{
	struct dma_coherent_mem *mem = dev->dma_mem;

	if (!mem)
		return;

	dmacoherent_region_del(dev);
	dma_release_coherent_memory(mem);
	dev->dma_mem = NULL;
}
EXPORT_SYMBOL(dma_release_declared_memory);

void *dma_mark_declared_memory_occupied(struct device *dev,
					dma_addr_t device_addr, size_t size)
{
	struct dma_coherent_mem *mem = dev->dma_mem;
	unsigned long flags;
	int pos, err;
	int order;

	if (!mem)
		return ERR_PTR(-EINVAL);

	size += device_addr & ~PAGE_MASK;
	order = get_order(size);
	pos = PFN_DOWN(device_addr - dma_get_device_base(dev, mem));

	spin_lock_irqsave(&mem->spinlock, flags);
	err = bitmap_allocate_region(mem->bitmap, pos, order);
	if (err != 0) {
		spin_unlock_irqrestore(&mem->spinlock, flags);
		return ERR_PTR(err);
	}
	mem->used += 1 << order;
	if (mem->highwatermark < mem->used)
		mem->highwatermark = mem->used;
	spin_unlock_irqrestore(&mem->spinlock, flags);
	return mem->virt_base + (pos << PAGE_SHIFT);
}
EXPORT_SYMBOL(dma_mark_declared_memory_occupied);

/**
 * dma_alloc_from_coherent() - try to allocate memory from the per-device coherent area
 *
 * @dev:	device from which we allocate memory
 * @size:	size of requested memory area
 * @dma_handle:	This will be filled with the correct dma handle
 * @ret:	This pointer will be filled with the virtual address
 *		to allocated area.
 *
 * This function should be only called from per-arch dma_alloc_coherent()
 * to support allocation from per-device coherent memory pools.
 *
 * Returns 0 if dma_alloc_coherent should continue with allocating from
 * generic memory areas, or !0 if dma_alloc_coherent should return @ret.
 */
int dma_alloc_from_coherent(struct device *dev, ssize_t size,
				       dma_addr_t *dma_handle, void **ret)
{
	struct dma_coherent_mem *mem = dev_get_coherent_memory(dev);
	int order = get_order(size);
	unsigned long flags;
	int pageno;
	int dma_memory_map;

	if (!mem)
		return 0;

	*ret = NULL;
	spin_lock_irqsave(&mem->spinlock, flags);

	if (unlikely(size > (mem->size << PAGE_SHIFT)))
		goto err;

	pageno = bitmap_find_free_region(mem->bitmap, mem->size, order);
	if (unlikely(pageno < 0))
		goto err;

	mem->used += 1 << order;
	if (mem->highwatermark < mem->used)
		mem->highwatermark = mem->used;

	/*
	 * Memory was found in the per-device area.
	 */
	*dma_handle = dma_get_device_base(dev, mem) + (pageno << PAGE_SHIFT);
	*ret = mem->virt_base + (pageno << PAGE_SHIFT);
	dma_memory_map = (mem->flags & DMA_MEMORY_MAP);
	spin_unlock_irqrestore(&mem->spinlock, flags);
	if (dma_memory_map)
		memset(*ret, 0, size);
	else
		memset_io(*ret, 0, size);

	return 1;

err:
	mem->errs++;
	spin_unlock_irqrestore(&mem->spinlock, flags);
	/*
	 * In the case where the allocation can not be satisfied from the
	 * per-device area, try to fall back to generic memory if the
	 * constraints allow it.
	 */
	return mem->flags & DMA_MEMORY_EXCLUSIVE;
}
EXPORT_SYMBOL(dma_alloc_from_coherent);

/**
 * dma_release_from_coherent() - try to free the memory allocated from per-device coherent memory pool
 * @dev:	device from which the memory was allocated
 * @order:	the order of pages allocated
 * @vaddr:	virtual address of allocated pages
 *
 * This checks whether the memory was allocated from the per-device
 * coherent memory pool and if so, releases that memory.
 *
 * Returns 1 if we correctly released the memory, or 0 if
 * dma_release_coherent() should proceed with releasing memory from
 * generic pools.
 */
int dma_release_from_coherent(struct device *dev, int order, void *vaddr)
{
	struct dma_coherent_mem *mem = dev_get_coherent_memory(dev);

	if (mem && vaddr >= mem->virt_base && vaddr <
		   (mem->virt_base + (mem->size << PAGE_SHIFT))) {
		int page = (vaddr - mem->virt_base) >> PAGE_SHIFT;
		unsigned long flags;

		spin_lock_irqsave(&mem->spinlock, flags);
		bitmap_release_region(mem->bitmap, page, order);
		mem->used -= 1 << order;
		spin_unlock_irqrestore(&mem->spinlock, flags);
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(dma_release_from_coherent);

/**
 * dma_mmap_from_coherent() - try to mmap the memory allocated from
 * per-device coherent memory pool to userspace
 * @dev:	device from which the memory was allocated
 * @vma:	vm_area for the userspace memory
 * @vaddr:	cpu address returned by dma_alloc_from_coherent
 * @size:	size of the memory buffer allocated by dma_alloc_from_coherent
 * @ret:	result from remap_pfn_range()
 *
 * This checks whether the memory was allocated from the per-device
 * coherent memory pool and if so, maps that memory to the provided vma.
 *
 * Returns 1 if we correctly mapped the memory, or 0 if the caller should
 * proceed with mapping memory from generic pools.
 */
int dma_mmap_from_coherent(struct device *dev, struct vm_area_struct *vma,
			   void *vaddr, size_t size, int *ret)
{
	struct dma_coherent_mem *mem = dev_get_coherent_memory(dev);

	if (mem && vaddr >= mem->virt_base && vaddr + size <=
		   (mem->virt_base + (mem->size << PAGE_SHIFT))) {
		unsigned long off = vma->vm_pgoff;
		int start = (vaddr - mem->virt_base) >> PAGE_SHIFT;
		int user_count = vma_pages(vma);
		int count = PAGE_ALIGN(size) >> PAGE_SHIFT;

		*ret = -ENXIO;
		if (off < count && user_count <= count - off) {
			unsigned long pfn = mem->pfn_base + start + off;
			*ret = remap_pfn_range(vma, vma->vm_start, pfn,
					       user_count << PAGE_SHIFT,
					       vma->vm_page_prot);
		}
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(dma_mmap_from_coherent);

/*
 * Support for reserved memory regions defined in device tree
 */
#ifdef CONFIG_OF_RESERVED_MEM
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>

static struct reserved_mem *dma_reserved_default_memory __initdata;

static int rmem_dma_device_init(struct reserved_mem *rmem, struct device *dev)
{
	struct dma_coherent_mem *mem = rmem->priv;

	if (!mem &&
	    !dma_init_coherent_memory(rmem->base, rmem->base, rmem->size,
				      DMA_MEMORY_MAP | DMA_MEMORY_EXCLUSIVE,
				      &mem)) {
		pr_err("Reserved memory: failed to init DMA memory pool at %pa, size %ld MiB\n",
			&rmem->base, (unsigned long)rmem->size / SZ_1M);
		return -ENODEV;
	}
	mem->use_dev_dma_pfn_offset = true;
	rmem->priv = mem;

	if (dmacoherent_region_add(dev))
		return -ENOMEM;

	dma_assign_coherent_memory(dev, mem);
	return 0;
}

static void rmem_dma_device_release(struct reserved_mem *rmem,
				    struct device *dev)
{
	if (dev) {
		dmacoherent_region_del(dev);
		dev->dma_mem = NULL;
	}
}

static const struct reserved_mem_ops rmem_dma_ops = {
	.device_init	= rmem_dma_device_init,
	.device_release	= rmem_dma_device_release,
};

static int __init rmem_dma_setup(struct reserved_mem *rmem)
{
	unsigned long node = rmem->fdt_node;

	if (of_get_flat_dt_prop(node, "reusable", NULL))
		return -EINVAL;

#ifdef CONFIG_ARM
	if (!of_get_flat_dt_prop(node, "no-map", NULL)) {
		pr_err("Reserved memory: regions without no-map are not yet supported\n");
		return -EINVAL;
	}

	if (of_get_flat_dt_prop(node, "linux,dma-default", NULL)) {
		WARN(dma_reserved_default_memory,
		     "Reserved memory: region for default DMA coherent area is redefined\n");
		dma_reserved_default_memory = rmem;
	}
#endif

	rmem->ops = &rmem_dma_ops;
	pr_info("Reserved memory: created DMA memory pool at %pa, size %ld MiB\n",
		&rmem->base, (unsigned long)rmem->size / SZ_1M);
	return 0;
}

static int __init dma_init_reserved_memory(void)
{
	const struct reserved_mem_ops *ops;
	int ret;

	if (!dma_reserved_default_memory)
		return -ENOMEM;

	ops = dma_reserved_default_memory->ops;

	/*
	 * We rely on rmem_dma_device_init() does not propagate error of
	 * dma_assign_coherent_memory() for "NULL" device.
	 */
	ret = ops->device_init(dma_reserved_default_memory, NULL);

	if (!ret) {
		dma_coherent_default_memory = dma_reserved_default_memory->priv;
		pr_info("DMA: default coherent area is set\n");
	}

	return ret;
}

core_initcall(dma_init_reserved_memory);

RESERVEDMEM_OF_DECLARE(dma, "shared-dma-pool", rmem_dma_setup);
#endif

#ifdef CONFIG_PROC_FS

static int dmainfo_proc_show_dma_mem(struct seq_file *m, void *v,
				     struct device *dev)
{
	struct dma_coherent_mem *mem = dev_get_coherent_memory(dev);
	int offset;
	int start;
	int end;
	int pages;
	int order;
	int free = 0;
	int blocks[MAX_ORDER];

	memset(blocks, 0, sizeof(blocks));

	spin_lock(&mem->spinlock);

	for (offset = 0; offset < mem->size; offset = end) {
		start = find_next_zero_bit(mem->bitmap, mem->size, offset);
		if (start >= mem->size)
			break;
		end = find_next_bit(mem->bitmap, mem->size, start + 1);
		pages = end - start;

		/* Align start: */
		for (order = 0; order < MAX_ORDER; order += 1) {
			if (start >= end)
				break;
			if (pages < (1 << order))
				break;
			if (start & (1 << order)) {
				blocks[order] += 1;
				start += 1 << order;
				pages -= 1 << order;
				free += 1 << order;
			}
		}

		if (start >= end)
			continue;

		/* Align middle and end: */
		order = MAX_ORDER - 1;
		while (order >= 0) {
			if (start >= end)
				break;
			if (pages >= (1 << order)) {
				blocks[order] += 1;
				start += 1 << order;
				pages -= 1 << order;
				free += 1 << order;
			} else {
				order -= 1;
			}
		}
	}

	seq_printf(m, "%-30s", dev_name(dev));

	for (order = 0; order < MAX_ORDER; order += 1)
		seq_printf(m, " %6d", blocks[order]);

	seq_printf(m, " %6d %6d %6d %6d %6d\n",
		   mem->size,
		   mem->used,
		   free,
		   mem->highwatermark,
		   mem->errs);

	spin_unlock(&mem->spinlock);

	return 0;
}

static int dmainfo_proc_show(struct seq_file *m, void *v)
{
	struct dmacoherent_region *rp;
	int order;

	seq_puts(m, "DMA-coherent region information:\n");
	seq_printf(m, "%-30s", "Free block count at order");

	for (order = 0; order < MAX_ORDER; ++order)
		seq_printf(m, " %6d", order);

	seq_printf(m, " %6s %6s %6s %6s %6s\n",
		   "Size",
		   "Used",
		   "Free",
		   "High",
		   "Errs");

	mutex_lock(&dmacoherent_region_list_lock);
	list_for_each_entry(rp, &dmacoherent_region_list, list) {
		dmainfo_proc_show_dma_mem(m, v, rp->dev);
	}
	mutex_unlock(&dmacoherent_region_list_lock);

	return 0;
}

static int dmainfo_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, dmainfo_proc_show, NULL);
}

static const struct file_operations dmainfo_proc_fops = {
	.open		= dmainfo_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init proc_dmainfo_init(void)
{
	proc_create("dmainfo", 0, NULL, &dmainfo_proc_fops);
	return 0;
}
module_init(proc_dmainfo_init);

#endif
