/*
 * kexec_file for arm64
 *
 * Copyright (C) 2017 Linaro Limited
 * Author: AKASHI Takahiro <takahiro.akashi@linaro.org>
 *
 * Most code is derived from arm64 port of kexec-tools
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) "kexec_file: " fmt

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kexec.h>
#include <linux/libfdt.h>
#include <linux/memblock.h>
#include <linux/of_fdt.h>
#include <asm/kexec_file.h>

static int __dt_root_addr_cells;
static int __dt_root_size_cells;

static struct kexec_file_ops *kexec_file_loaders[0];

int arch_kexec_kernel_image_probe(struct kimage *image, void *buf,
				  unsigned long buf_len)
{
	struct kexec_file_ops *fops;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(kexec_file_loaders); i++) {
		fops = kexec_file_loaders[i];
		if (!fops || !fops->probe)
			continue;

		ret = fops->probe(buf, buf_len);
		if (!ret) {
			image->fops = fops;
			return 0;
		}
	}

	return -ENOEXEC;
}

void *arch_kexec_kernel_image_load(struct kimage *image)
{
	if (!image->fops || !image->fops->load)
		return ERR_PTR(-ENOEXEC);

	return image->fops->load(image, image->kernel_buf,
				 image->kernel_buf_len, image->initrd_buf,
				 image->initrd_buf_len, image->cmdline_buf,
				 image->cmdline_buf_len);
}

int arch_kimage_file_post_load_cleanup(struct kimage *image)
{
	vfree(image->arch.dtb_buf);
	image->arch.dtb_buf = NULL;

	vfree(image->arch.elf_headers);
	image->arch.elf_headers = NULL;
	image->arch.elf_headers_sz = 0;

	if (!image->fops || !image->fops->cleanup)
		return 0;

	return image->fops->cleanup(image->image_loader_data);
}

#ifdef CONFIG_KEXEC_VERIFY_SIG
int arch_kexec_kernel_verify_sig(struct kimage *image, void *kernel,
				 unsigned long kernel_len)
{
	if (!image->fops || !image->fops->verify_sig) {
		pr_debug("kernel loader does not support signature verification.\n");
		return -EKEYREJECTED;
	}

	return image->fops->verify_sig(kernel, kernel_len);
}
#endif

int arch_kexec_walk_mem(struct kexec_buf *kbuf, int (*func)(u64, u64, void *))
{
	if (kbuf->image->type == KEXEC_TYPE_CRASH)
		return walk_iomem_res_desc(crashk_res.desc,
					IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY,
					crashk_res.start, crashk_res.end,
					kbuf, func);
	else if (kbuf->top_down)
		return walk_system_ram_res_rev(0, ULONG_MAX, kbuf, func);
	else
		return walk_system_ram_res(0, ULONG_MAX, kbuf, func);
}

static int __init arch_kexec_file_init(void)
{
	/* Those values are used later on loading the kernel */
	__dt_root_addr_cells = dt_root_addr_cells;
	__dt_root_size_cells = dt_root_size_cells;

	return 0;
}
late_initcall(arch_kexec_file_init);

#define FDT_ALIGN(x, a)	(((x) + (a) - 1) & ~((a) - 1))
#define FDT_TAGALIGN(x)	(FDT_ALIGN((x), FDT_TAGSIZE))

static int fdt_prop_len(const char *prop_name, int len)
{
	return (strlen(prop_name) + 1) +
		sizeof(struct fdt_property) +
		FDT_TAGALIGN(len);
}

static bool cells_size_fitted(unsigned long base, unsigned long size)
{
	/* if *_cells >= 2, cells can hold 64-bit values anyway */
	if ((__dt_root_addr_cells == 1) && (base >= (1ULL << 32)))
		return false;

	if ((__dt_root_size_cells == 1) && (size >= (1ULL << 32)))
		return false;

	return true;
}

static void fill_property(void *buf, u64 val64, int cells)
{
	u32 val32;
	int i;

	if (cells == 1) {
		val32 = cpu_to_fdt32((u32)val64);
		memcpy(buf, &val32, sizeof(val32));
	} else {
		for (i = 0; i < (cells * sizeof(u32) - sizeof(u64)); i++)
			*(char *)buf++ = 0;

		val64 = cpu_to_fdt64(val64);
		memcpy(buf, &val64, sizeof(val64));
	}
}

static int fdt_setprop_range(void *fdt, int nodeoffset, const char *name,
				unsigned long addr, unsigned long size)
{
	u64 range[2];
	void *prop;
	size_t buf_size;
	int result;

	prop = range;
	buf_size = (__dt_root_addr_cells + __dt_root_size_cells) * sizeof(u32);

	fill_property(prop, addr, __dt_root_addr_cells);
	prop += __dt_root_addr_cells * sizeof(u32);

	fill_property(prop, size, __dt_root_size_cells);
	prop += __dt_root_size_cells * sizeof(u32);

	result = fdt_setprop(fdt, nodeoffset, name, range, buf_size);

	return result;
}

int setup_dtb(struct kimage *image,
		unsigned long initrd_load_addr, unsigned long initrd_len,
		char *cmdline, unsigned long cmdline_len,
		char **dtb_buf, size_t *dtb_buf_len)
{
	char *buf = NULL;
	size_t buf_size;
	int nodeoffset;
	u64 value;
	int range_len;
	int ret;

	/* check ranges against root's #address-cells and #size-cells */
	if (image->type == KEXEC_TYPE_CRASH &&
		(!cells_size_fitted(image->arch.elf_load_addr,
				image->arch.elf_headers_sz) ||
		 !cells_size_fitted(crashk_res.start,
				crashk_res.end - crashk_res.start + 1))) {
		pr_err("Crash memory region doesn't fit into DT's root cell sizes.\n");
		ret = -EINVAL;
		goto out_err;
	}

	/* duplicate dt blob */
	buf_size = fdt_totalsize(initial_boot_params);
	range_len = (__dt_root_addr_cells + __dt_root_size_cells) * sizeof(u32);

	if (image->type == KEXEC_TYPE_CRASH)
		buf_size += fdt_prop_len("linux,elfcorehdr", range_len)
				+ fdt_prop_len("linux,usable-memory-range",
								range_len);

	if (initrd_load_addr)
		buf_size += fdt_prop_len("initrd-start", sizeof(u64))
				+ fdt_prop_len("initrd-end", sizeof(u64));

	if (cmdline)
		buf_size += fdt_prop_len("bootargs", cmdline_len + 1);

	buf = vmalloc(buf_size);
	if (!buf) {
		ret = -ENOMEM;
		goto out_err;
	}

	ret = fdt_open_into(initial_boot_params, buf, buf_size);
	if (ret)
		goto out_err;

	nodeoffset = fdt_path_offset(buf, "/chosen");
	if (nodeoffset < 0)
		goto out_err;

	if (image->type == KEXEC_TYPE_CRASH) {
		/* add linux,elfcorehdr */
		ret = fdt_setprop_range(buf, nodeoffset, "linux,elfcorehdr",
				image->arch.elf_load_addr,
				image->arch.elf_headers_sz);
		if (ret)
			goto out_err;

		/* add linux,usable-memory-range */
		ret = fdt_setprop_range(buf, nodeoffset,
				"linux,usable-memory-range",
				crashk_res.start,
				crashk_res.end - crashk_res.start + 1);
		if (ret)
			goto out_err;
	}

	/* add bootargs */
	if (cmdline) {
		ret = fdt_setprop(buf, nodeoffset, "bootargs",
						cmdline, cmdline_len + 1);
		if (ret)
			goto out_err;
	}

	/* add initrd-* */
	if (initrd_load_addr) {
		value = cpu_to_fdt64(initrd_load_addr);
		ret = fdt_setprop(buf, nodeoffset, "initrd-start",
				&value, sizeof(value));
		if (ret)
			goto out_err;

		value = cpu_to_fdt64(initrd_load_addr + initrd_len);
		ret = fdt_setprop(buf, nodeoffset, "initrd-end",
				&value, sizeof(value));
		if (ret)
			goto out_err;
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

int load_other_segments(struct kimage *image, unsigned long kernel_load_addr,
			char *initrd, unsigned long initrd_len,
			char *cmdline, unsigned long cmdline_len)
{
	struct kexec_buf kbuf;
	unsigned long initrd_load_addr = 0;
	unsigned long purgatory_load_addr, dtb_load_addr;
	char *dtb = NULL;
	unsigned long dtb_len;
	int ret = 0;

	kbuf.image = image;
	/* not allocate anything below the kernel */
	kbuf.buf_min = kernel_load_addr;

	/* Load initrd */
	if (initrd) {
		kbuf.buffer = initrd;
		kbuf.bufsz = initrd_len;
		kbuf.memsz = initrd_len;
		kbuf.buf_align = PAGE_SIZE;
		/* within 1GB-aligned window of up to 32GB in size */
		kbuf.buf_max = round_down(kernel_load_addr, SZ_1G)
						+ (unsigned long)SZ_1G * 31;
		kbuf.top_down = 0;

		ret = kexec_add_buffer(&kbuf);
		if (ret)
			goto out_err;
		initrd_load_addr = kbuf.mem;

		pr_debug("Loaded initrd at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
				initrd_load_addr, initrd_len, initrd_len);
	}

	/* Load dtb blob */
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
	kbuf.top_down = 1;

	ret = kexec_add_buffer(&kbuf);
	if (ret)
		goto out_err;
	dtb_load_addr = kbuf.mem;
	image->arch.dtb_buf = dtb;

	pr_debug("Loaded dtb at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
			dtb_load_addr, dtb_len, dtb_len);

	/* Load purgatory  */
	ret = kexec_load_purgatory(image, kernel_load_addr, ULONG_MAX, 1,
				   &purgatory_load_addr);
	if (ret) {
		pr_err("Loading purgatory failed\n");
		goto out_err;
	}

	ret = kexec_purgatory_get_set_symbol(image, "kernel_entry",
				&kernel_load_addr, sizeof(kernel_load_addr), 0);
	if (ret) {
		pr_err("Setting symbol (kernel_entry) failed.\n");
		goto out_err;
	}

	ret = kexec_purgatory_get_set_symbol(image, "dtb_addr",
				&dtb_load_addr, sizeof(dtb_load_addr), 0);
	if (ret) {
		pr_err("Setting symbol (dtb_addr) failed.\n");
		goto out_err;
	}

	pr_debug("Loaded purgatory at 0x%lx\n", purgatory_load_addr);

	return 0;

out_err:
	vfree(dtb);
	image->arch.dtb_buf = NULL;
	return ret;
}

int load_crashdump_segments(struct kimage *image)
{
	void *elf_addr;
	unsigned long elf_sz;
	struct kexec_buf kbuf;
	int ret;

	if (image->type != KEXEC_TYPE_CRASH)
		return 0;

	/* Prepare elf headers and add a segment */
	ret = prepare_elf_headers(image, &elf_addr, &elf_sz);
	if (ret) {
		pr_err("Preparing elf core header failed\n");
		return ret;
	}

	kbuf.image = image;
	kbuf.buffer = elf_addr;
	kbuf.bufsz = elf_sz;
	kbuf.memsz = elf_sz;
	kbuf.buf_align = PAGE_SIZE;
	kbuf.buf_min = crashk_res.start;
	kbuf.buf_max = crashk_res.end + 1;
	kbuf.top_down = 1;

	ret = kexec_add_buffer(&kbuf);
	if (ret) {
		vfree(elf_addr);
		return ret;
	}
	image->arch.elf_headers = elf_addr;
	image->arch.elf_headers_sz = elf_sz;
	image->arch.elf_load_addr = kbuf.mem;

	pr_debug("Loaded elf core header at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
			 image->arch.elf_load_addr, elf_sz, elf_sz);

	return ret;
}
