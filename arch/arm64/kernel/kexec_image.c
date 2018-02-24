/*
 * Kexec image loader

 * Copyright (C) 2018 Linaro Limited
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
#include <asm/byteorder.h>
#include <asm/memory.h>

static int image_probe(const char *kernel_buf, unsigned long kernel_len)
{
	const struct arm64_image_header *h;

	h = (const struct arm64_image_header *)(kernel_buf);

	if ((kernel_len < sizeof(*h)) || !arm64_header_check_magic(h))
		return -EINVAL;

	return 0;
}

static void *image_load(struct kimage *image, char *kernel,
			    unsigned long kernel_len, char *initrd,
			    unsigned long initrd_len, char *cmdline,
			    unsigned long cmdline_len)
{
	struct kexec_buf kbuf;
	struct arm64_image_header *h = (struct arm64_image_header *)kernel;
	unsigned long text_offset;
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
	image->start = kbuf.mem + text_offset;

	pr_debug("Loaded kernel at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
		 image->start, kbuf.bufsz, kbuf.memsz);

	/* Load additional data */
	ret = load_other_segments(image, image->start,
			    initrd, initrd_len, cmdline, cmdline_len);

out:
	return ERR_PTR(ret);
}

const struct kexec_file_ops kexec_image_ops = {
	.probe = image_probe,
	.load = image_load,
};
