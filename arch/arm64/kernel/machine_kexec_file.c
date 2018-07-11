// SPDX-License-Identifier: GPL-2.0
/*
 * kexec_file for arm64
 *
 * Copyright (C) 2018 Linaro Limited
 * Author: AKASHI Takahiro <takahiro.akashi@linaro.org>
 *
 * Most code is derived from arm64 port of kexec-tools
 */

#define pr_fmt(fmt) "kexec_file: " fmt

#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/kexec.h>
#include <linux/libfdt.h>
#include <linux/memblock.h>
#include <linux/of_fdt.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <asm/byteorder.h>

const struct kexec_file_ops * const kexec_file_loaders[] = {
	&kexec_image_ops,
	NULL
};

int arch_kimage_file_post_load_cleanup(struct kimage *image)
{
	vfree(image->arch.dtb_buf);
	image->arch.dtb_buf = NULL;

	vfree(image->arch.elf_headers);
	image->arch.elf_headers = NULL;
	image->arch.elf_headers_sz = 0;

	return kexec_image_post_load_cleanup_default(image);
}

static int setup_dtb(struct kimage *image,
		unsigned long initrd_load_addr, unsigned long initrd_len,
		char *cmdline, unsigned long cmdline_len,
		char **dtb_buf, size_t *dtb_buf_len)
{
	char *buf = NULL;
	size_t buf_size, range_size;
	int nodeoffset;
	u64 value;
	int ret;

	/* check ranges against root's #address-cells and #size-cells */
	if (image->type == KEXEC_TYPE_CRASH &&
		(!of_fdt_cells_size_fitted(image->arch.elf_load_addr,
				image->arch.elf_headers_sz) ||
		 !of_fdt_cells_size_fitted(crashk_res.start,
				crashk_res.end - crashk_res.start + 1))) {
		pr_err("Crash memory region doesn't fit into DT's root cell sizes.\n");
		ret = -EINVAL;
		goto out_err;
	}

	/* duplicate dt blob */
	buf_size = fdt_totalsize(initial_boot_params);
	range_size = of_fdt_reg_cells_size();

	if (image->type == KEXEC_TYPE_CRASH) {
		buf_size += fdt_prop_len("linux,elfcorehdr", range_size);
		buf_size += fdt_prop_len("linux,usable-memory-range",
								range_size);
	}

	if (initrd_load_addr) {
		/* can be redundant, but trimmed at the end */
		buf_size += fdt_prop_len("linux,initrd-start", sizeof(u64));
		buf_size += fdt_prop_len("linux,initrd-end", sizeof(u64));
	}

	if (cmdline)
		/* can be redundant, but trimmed at the end */
		buf_size += fdt_prop_len("bootargs", cmdline_len + 1);

	buf = vmalloc(buf_size);
	if (!buf) {
		ret = -ENOMEM;
		goto out_err;
	}

	ret = fdt_open_into(initial_boot_params, buf, buf_size);
	if (ret) {
		ret = -EINVAL;
		goto out_err;
	}

	nodeoffset = fdt_path_offset(buf, "/chosen");
	if (nodeoffset < 0) {
		ret = -EINVAL;
		goto out_err;
	}

	if (image->type == KEXEC_TYPE_CRASH) {
		/* add linux,elfcorehdr */
		ret = fdt_setprop_reg(buf, nodeoffset, "linux,elfcorehdr",
				image->arch.elf_load_addr,
				image->arch.elf_headers_sz);
		if (ret)
			goto out_err;

		/* add linux,usable-memory-range */
		ret = fdt_setprop_reg(buf, nodeoffset,
				"linux,usable-memory-range",
				crashk_res.start,
				crashk_res.end - crashk_res.start + 1);
		if (ret)
			goto out_err;
	}

	/* add bootargs */
	if (cmdline) {
		ret = fdt_setprop_string(buf, nodeoffset, "bootargs", cmdline);
		if (ret) {
			ret = -EINVAL;
			goto out_err;
		}
	} else {
		ret = fdt_delprop(buf, nodeoffset, "bootargs");
		if (ret && (ret != -FDT_ERR_NOTFOUND)) {
			ret = -EINVAL;
			goto out_err;
		}
	}

	/* add initrd-* */
	if (initrd_load_addr) {
		value = cpu_to_fdt64(initrd_load_addr);
		ret = fdt_setprop_u64(buf, nodeoffset, "linux,initrd-start",
							value);
		if (ret) {
			ret = -EINVAL;
			goto out_err;
		}

		value = cpu_to_fdt64(initrd_load_addr + initrd_len);
		ret = fdt_setprop_u64(buf, nodeoffset, "linux,initrd-end",
							value);
		if (ret) {
			ret = -EINVAL;
			goto out_err;
		}
	} else {
		ret = fdt_delprop(buf, nodeoffset, "linux,initrd-start");
		if (ret && (ret != -FDT_ERR_NOTFOUND)) {
			ret = -EINVAL;
			goto out_err;
		}

		ret = fdt_delprop(buf, nodeoffset, "linux,initrd-end");
		if (ret && (ret != -FDT_ERR_NOTFOUND)) {
			ret = -EINVAL;
			goto out_err;
		}
	}

	/* trim a buffer */
	fdt_pack(buf);
	*dtb_buf = buf;
	*dtb_buf_len = fdt_totalsize(buf);

	return 0;

out_err:
	vfree(buf);
	return ret;
}

static int prepare_elf_headers(void **addr, unsigned long *sz)
{
	struct crash_mem *cmem;
	unsigned int nr_ranges;
	int ret;
	u64 i;
	phys_addr_t start, end;

	nr_ranges = 1; /* for exclusion of crashkernel region */
	for_each_mem_range(i, &memblock.memory, NULL, NUMA_NO_NODE, 0,
							&start, &end, NULL)
		nr_ranges++;

	cmem = kmalloc(sizeof(struct crash_mem) +
			sizeof(struct crash_mem_range) * nr_ranges, GFP_KERNEL);
	if (!cmem)
		return -ENOMEM;

	cmem->max_nr_ranges = nr_ranges;
	cmem->nr_ranges = 0;
	for_each_mem_range(i, &memblock.memory, NULL, NUMA_NO_NODE, 0,
							&start, &end, NULL) {
		cmem->ranges[cmem->nr_ranges].start = start;
		cmem->ranges[cmem->nr_ranges].end = end - 1;
		cmem->nr_ranges++;
	}

	/* Exclude crashkernel region */
	ret = crash_exclude_mem_range(cmem, crashk_res.start, crashk_res.end);
	if (ret)
		goto out;

	ret =  crash_prepare_elf64_headers(cmem, true, addr, sz);

out:
	kfree(cmem);
	return ret;
}

int load_other_segments(struct kimage *image,
			unsigned long kernel_load_addr,
			unsigned long kernel_size,
			char *initrd, unsigned long initrd_len,
			char *cmdline, unsigned long cmdline_len)
{
	struct kexec_buf kbuf;
	void *hdrs_addr;
	unsigned long hdrs_sz;
	unsigned long initrd_load_addr = 0;
	char *dtb = NULL;
	unsigned long dtb_len = 0;
	int ret = 0;

	/* load elf core header */
	if (image->type == KEXEC_TYPE_CRASH) {
		ret = prepare_elf_headers(&hdrs_addr, &hdrs_sz);
		if (ret) {
			pr_err("Preparing elf core header failed\n");
			goto out_err;
		}

		kbuf.image = image;
		kbuf.buffer = hdrs_addr;
		kbuf.bufsz = hdrs_sz;
		kbuf.memsz = hdrs_sz;
		kbuf.buf_align = PAGE_SIZE;
		kbuf.buf_min = crashk_res.start;
		kbuf.buf_max = crashk_res.end + 1;
		kbuf.top_down = true;

		ret = kexec_add_buffer(&kbuf);
		if (ret) {
			vfree(hdrs_addr);
			goto out_err;
		}
		image->arch.elf_headers = hdrs_addr;
		image->arch.elf_headers_sz = hdrs_sz;
		image->arch.elf_load_addr = kbuf.mem;

		pr_debug("Loaded elf core header at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
				 image->arch.elf_load_addr, hdrs_sz, hdrs_sz);
	}

	kbuf.image = image;
	/* not allocate anything below the kernel */
	kbuf.buf_min = kernel_load_addr + kernel_size;

	/* load initrd */
	if (initrd) {
		kbuf.buffer = initrd;
		kbuf.bufsz = initrd_len;
		kbuf.memsz = initrd_len;
		kbuf.buf_align = 0;
		/* within 1GB-aligned window of up to 32GB in size */
		kbuf.buf_max = round_down(kernel_load_addr, SZ_1G)
						+ (unsigned long)SZ_1G * 32;
		kbuf.top_down = false;

		ret = kexec_add_buffer(&kbuf);
		if (ret)
			goto out_err;
		initrd_load_addr = kbuf.mem;

		pr_debug("Loaded initrd at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
				initrd_load_addr, initrd_len, initrd_len);
	}

	/* load dtb blob */
	ret = setup_dtb(image, initrd_load_addr, initrd_len,
				cmdline, cmdline_len, &dtb, &dtb_len);
	if (ret) {
		pr_err("Preparing for new dtb failed\n");
		goto out_err;
	}

	kbuf.buffer = dtb;
	kbuf.bufsz = dtb_len;
	kbuf.memsz = dtb_len;
	/* not across 2MB boundary */
	kbuf.buf_align = SZ_2M;
	kbuf.buf_max = ULONG_MAX;
	kbuf.top_down = true;

	ret = kexec_add_buffer(&kbuf);
	if (ret)
		goto out_err;
	image->arch.dtb_mem = kbuf.mem;
	image->arch.dtb_buf = dtb;

	pr_debug("Loaded dtb at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
			kbuf.mem, dtb_len, dtb_len);

	return 0;

out_err:
	vfree(dtb);
	return ret;
}
