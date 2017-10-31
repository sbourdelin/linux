/*
 *  tpacket v4
 *  Copyright(c) 2017 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef _LINUX_TPACKET4_H
#define _LINUX_TPACKET4_H

#define TP4_UMEM_MIN_FRAME_SIZE 2048
#define TP4_KERNEL_HEADROOM 256 /* Headrom for XDP */

struct tp4_umem {
	struct pid *pid;
	struct page **pgs;
	unsigned int npgs;
	size_t size;
	unsigned long address;
	unsigned int frame_size;
	unsigned int frame_size_log2;
	unsigned int nframes;
	unsigned int nfpplog2; /* num frames per page in log2 */
	unsigned int data_headroom;
};

/*************** V4 QUEUE OPERATIONS *******************************/

/**
 * tp4q_umem_new - Creates a new umem (packet buffer)
 *
 * @addr: The address to the umem
 * @size: The size of the umem
 * @frame_size: The size of each frame, between 2K and PAGE_SIZE
 * @data_headroom: The desired data headroom before start of the packet
 *
 * Returns a pointer to the new umem or NULL for failure
 **/
static inline struct tp4_umem *tp4q_umem_new(unsigned long addr, size_t size,
					     unsigned int frame_size,
					     unsigned int data_headroom)
{
	struct tp4_umem *umem;
	unsigned int nframes;

	if (frame_size < TP4_UMEM_MIN_FRAME_SIZE || frame_size > PAGE_SIZE) {
		/* Strictly speaking we could support this, if:
		 * - huge pages, or*
		 * - using an IOMMU, or
		 * - making sure the memory area is consecutive
		 * but for now, we simply say "computer says no".
		 */
		return ERR_PTR(-EINVAL);
	}

	if (!is_power_of_2(frame_size))
		return ERR_PTR(-EINVAL);

	if (!PAGE_ALIGNED(addr)) {
		/* Memory area has to be page size aligned. For
		 * simplicity, this might change.
		 */
		return ERR_PTR(-EINVAL);
	}

	if ((addr + size) < addr)
		return ERR_PTR(-EINVAL);

	nframes = size / frame_size;
	if (nframes == 0)
		return ERR_PTR(-EINVAL);

	data_headroom =	ALIGN(data_headroom, 64);

	if (frame_size - data_headroom - TP4_KERNEL_HEADROOM < 0)
		return ERR_PTR(-EINVAL);

	umem = kzalloc(sizeof(*umem), GFP_KERNEL);
	if (!umem)
		return ERR_PTR(-ENOMEM);

	umem->pid = get_task_pid(current, PIDTYPE_PID);
	umem->size = size;
	umem->address = addr;
	umem->frame_size = frame_size;
	umem->frame_size_log2 = ilog2(frame_size);
	umem->nframes = nframes;
	umem->nfpplog2 = ilog2(PAGE_SIZE / frame_size);
	umem->data_headroom = data_headroom;

	return umem;
}

#endif /* _LINUX_TPACKET4_H */
