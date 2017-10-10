/*
 * Kexec image loader

 * Copyright (C) 2017 Linaro Limited
 * Author: AKASHI Takahiro <takahiro.akashi@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt)	"kexec_file(Image): " fmt

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kexec.h>
#include <linux/verification.h>
#include <asm/byteorder.h>
#include <asm/memory.h>

static int image_probe(const char *kernel_buf, unsigned long kernel_len)
{
	const struct arm64_image_header *h;

	h = (const struct arm64_image_header *)(kernel_buf);

	if ((kernel_len < sizeof(*h)) || !arm64_header_check_magic(h))
		return -EINVAL;

	pr_debug("PE format: %s\n",
			(arm64_header_check_pe_sig(h) ? "yes" : "no"));

	return 0;
}

static void *image_load(struct kimage *image, char *kernel,
			    unsigned long kernel_len, char *initrd,
			    unsigned long initrd_len, char *cmdline,
			    unsigned long cmdline_len)
{
	struct kexec_buf kbuf;
	struct arm64_image_header *h = (struct arm64_image_header *)kernel;
	unsigned long text_offset, kernel_load_addr;
	int ret;

	/* Create elf core header segment */
	ret = load_crashdump_segments(image);
	if (ret)
		goto out;

	/* Load the kernel */
	kbuf.image = image;
	if (image->type == KEXEC_TYPE_CRASH) {
		kbuf.buf_min = crashk_res.start;
		kbuf.buf_max = crashk_res.end + 1;
	} else {
		kbuf.buf_min = 0;
		kbuf.buf_max = ULONG_MAX;
	}
	kbuf.top_down = 0;

	kbuf.buffer = kernel;
	kbuf.bufsz = kernel_len;
	kbuf.memsz = le64_to_cpu(h->image_size);
	text_offset = le64_to_cpu(h->text_offset);
	kbuf.buf_align = SZ_2M;

	/* Adjust kernel segment with TEXT_OFFSET */
	kbuf.memsz += text_offset;

	ret = kexec_add_buffer(&kbuf);
	if (ret)
		goto out;

	image->segment[image->nr_segments - 1].mem += text_offset;
	image->segment[image->nr_segments - 1].memsz -= text_offset;
	kernel_load_addr = kbuf.mem + text_offset;

	pr_debug("Loaded kernel at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
		 kernel_load_addr, kbuf.bufsz, kbuf.memsz);

	/* Load additional data */
	ret = load_other_segments(image, kernel_load_addr,
			    initrd, initrd_len, cmdline, cmdline_len);

out:
	return ERR_PTR(ret);
}

#ifdef CONFIG_KEXEC_VERIFY_SIG
static int image_verify_sig(const char *kernel, unsigned long kernel_len)
{
	return verify_pefile_signature(kernel, kernel_len, NULL,
				       VERIFYING_KEXEC_PE_SIGNATURE);
}
#endif

const struct kexec_file_ops kexec_image_ops = {
	.probe = image_probe,
	.load = image_load,
#ifdef CONFIG_KEXEC_VERIFY_SIG
	.verify_sig = image_verify_sig,
#endif
};
