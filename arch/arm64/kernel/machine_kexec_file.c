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
#include <linux/types.h>
#include <asm/byteorder.h>

/* relevant device tree properties */
#define FDT_PSTR_INITRD_STA	"linux,initrd-start"
#define FDT_PSTR_INITRD_END	"linux,initrd-end"
#define FDT_PSTR_BOOTARGS	"bootargs"

const struct kexec_file_ops * const kexec_file_loaders[] = {
	NULL
};

int arch_kimage_file_post_load_cleanup(struct kimage *image)
{
	vfree(image->arch.dtb);
	image->arch.dtb = NULL;

	return kexec_image_post_load_cleanup_default(image);
}

static int setup_dtb(struct kimage *image,
		unsigned long initrd_load_addr, unsigned long initrd_len,
		char *cmdline, unsigned long cmdline_len,
		void **dtb_buf, unsigned long *dtb_buf_len)
{
	void *buf = NULL;
	size_t buf_size;
	int nodeoffset;
	int ret;

	/* duplicate dt blob */
	buf_size = fdt_totalsize(initial_boot_params);

	if (initrd_load_addr) {
		/* can be redundant, but trimmed at the end */
		buf_size += fdt_prop_len(FDT_PSTR_INITRD_STA, sizeof(u64));
		buf_size += fdt_prop_len(FDT_PSTR_INITRD_END, sizeof(u64));
	}

	if (cmdline)
		/* can be redundant, but trimmed at the end */
		buf_size += fdt_prop_len(FDT_PSTR_BOOTARGS, cmdline_len);

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

	/* add bootargs */
	if (cmdline) {
		ret = fdt_setprop_string(buf, nodeoffset, FDT_PSTR_BOOTARGS,
							cmdline);
		if (ret) {
			ret = -EINVAL;
			goto out_err;
		}
	} else {
		ret = fdt_delprop(buf, nodeoffset, FDT_PSTR_BOOTARGS);
		if (ret && (ret != -FDT_ERR_NOTFOUND)) {
			ret = -EINVAL;
			goto out_err;
		}
	}

	/* add initrd-* */
	if (initrd_load_addr) {
		ret = fdt_setprop_u64(buf, nodeoffset, FDT_PSTR_INITRD_STA,
							initrd_load_addr);
		if (ret) {
			ret = -EINVAL;
			goto out_err;
		}

		ret = fdt_setprop_u64(buf, nodeoffset, FDT_PSTR_INITRD_END,
						initrd_load_addr + initrd_len);
		if (ret) {
			ret = -EINVAL;
			goto out_err;
		}
	} else {
		ret = fdt_delprop(buf, nodeoffset, FDT_PSTR_INITRD_STA);
		if (ret && (ret != -FDT_ERR_NOTFOUND)) {
			ret = -EINVAL;
			goto out_err;
		}

		ret = fdt_delprop(buf, nodeoffset, FDT_PSTR_INITRD_END);
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

int load_other_segments(struct kimage *image,
			unsigned long kernel_load_addr,
			unsigned long kernel_size,
			char *initrd, unsigned long initrd_len,
			char *cmdline, unsigned long cmdline_len)
{
	struct kexec_buf kbuf;
	void *dtb = NULL;
	unsigned long initrd_load_addr = 0, dtb_len;
	int ret = 0;

	kbuf.image = image;
	/* not allocate anything below the kernel */
	kbuf.buf_min = kernel_load_addr + kernel_size;

	/* load initrd */
	if (initrd) {
		kbuf.buffer = initrd;
		kbuf.bufsz = initrd_len;
		kbuf.mem = 0;
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
	kbuf.mem = 0;
	kbuf.memsz = dtb_len;
	/* not across 2MB boundary */
	kbuf.buf_align = SZ_2M;
	kbuf.buf_max = ULONG_MAX;
	kbuf.top_down = true;

	ret = kexec_add_buffer(&kbuf);
	if (ret)
		goto out_err;
	image->arch.dtb = dtb;
	image->arch.dtb_mem = kbuf.mem;

	pr_debug("Loaded dtb at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
			kbuf.mem, dtb_len, dtb_len);

	return 0;

out_err:
	vfree(dtb);
	return ret;
}
