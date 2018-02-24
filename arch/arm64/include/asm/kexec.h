/*
 * kexec for arm64
 *
 * Copyright (C) Linaro.
 * Copyright (C) Huawei Futurewei Technologies.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _ARM64_KEXEC_H
#define _ARM64_KEXEC_H

/* Maximum physical address we can use pages from */

#define KEXEC_SOURCE_MEMORY_LIMIT (-1UL)

/* Maximum address we can reach in physical address mode */

#define KEXEC_DESTINATION_MEMORY_LIMIT (-1UL)

/* Maximum address we can use for the control code buffer */

#define KEXEC_CONTROL_MEMORY_LIMIT (-1UL)

#define KEXEC_CONTROL_PAGE_SIZE 4096

#define KEXEC_ARCH KEXEC_ARCH_AARCH64

#ifndef __ASSEMBLY__

/**
 * crash_setup_regs() - save registers for the panic kernel
 *
 * @newregs: registers are saved here
 * @oldregs: registers to be saved (may be %NULL)
 */

static inline void crash_setup_regs(struct pt_regs *newregs,
				    struct pt_regs *oldregs)
{
	if (oldregs) {
		memcpy(newregs, oldregs, sizeof(*newregs));
	} else {
		u64 tmp1, tmp2;

		__asm__ __volatile__ (
			"stp	 x0,   x1, [%2, #16 *  0]\n"
			"stp	 x2,   x3, [%2, #16 *  1]\n"
			"stp	 x4,   x5, [%2, #16 *  2]\n"
			"stp	 x6,   x7, [%2, #16 *  3]\n"
			"stp	 x8,   x9, [%2, #16 *  4]\n"
			"stp	x10,  x11, [%2, #16 *  5]\n"
			"stp	x12,  x13, [%2, #16 *  6]\n"
			"stp	x14,  x15, [%2, #16 *  7]\n"
			"stp	x16,  x17, [%2, #16 *  8]\n"
			"stp	x18,  x19, [%2, #16 *  9]\n"
			"stp	x20,  x21, [%2, #16 * 10]\n"
			"stp	x22,  x23, [%2, #16 * 11]\n"
			"stp	x24,  x25, [%2, #16 * 12]\n"
			"stp	x26,  x27, [%2, #16 * 13]\n"
			"stp	x28,  x29, [%2, #16 * 14]\n"
			"mov	 %0,  sp\n"
			"stp	x30,  %0,  [%2, #16 * 15]\n"

			"/* faked current PSTATE */\n"
			"mrs	 %0, CurrentEL\n"
			"mrs	 %1, SPSEL\n"
			"orr	 %0, %0, %1\n"
			"mrs	 %1, DAIF\n"
			"orr	 %0, %0, %1\n"
			"mrs	 %1, NZCV\n"
			"orr	 %0, %0, %1\n"
			/* pc */
			"adr	 %1, 1f\n"
		"1:\n"
			"stp	 %1, %0,   [%2, #16 * 16]\n"
			: "=&r" (tmp1), "=&r" (tmp2)
			: "r" (newregs)
			: "memory"
		);
	}
}

#if defined(CONFIG_KEXEC_CORE) && defined(CONFIG_HIBERNATION)
extern bool crash_is_nosave(unsigned long pfn);
extern void crash_prepare_suspend(void);
extern void crash_post_resume(void);
#else
static inline bool crash_is_nosave(unsigned long pfn) {return false; }
static inline void crash_prepare_suspend(void) {}
static inline void crash_post_resume(void) {}
#endif

#ifdef CONFIG_KEXEC_FILE
#define ARCH_HAS_KIMAGE_ARCH

struct kimage_arch {
	phys_addr_t dtb_mem;
	void *dtb_buf;
	/* Core ELF header buffer */
	void *elf_headers;
	unsigned long elf_headers_sz;
	unsigned long elf_load_addr;
};

/**
 * struct arm64_image_header - arm64 kernel image header
 *
 * @pe_sig: Optional PE format 'MZ' signature
 * @branch_code: Instruction to branch to stext
 * @text_offset: Image load offset, little endian
 * @image_size: Effective image size, little endian
 * @flags:
 *	Bit 0: Kernel endianness. 0=little endian, 1=big endian
 * @reserved: Reserved
 * @magic: Magic number, "ARM\x64"
 * @pe_header: Optional offset to a PE format header
 **/

struct arm64_image_header {
	u8 pe_sig[2];
	u8 pad[2];
	u32 branch_code;
	u64 text_offset;
	u64 image_size;
	u64 flags;
	u64 reserved[3];
	u8 magic[4];
	u32 pe_header;
};

static const u8 arm64_image_magic[4] = {'A', 'R', 'M', 0x64U};
static const u8 arm64_image_pe_sig[2] = {'M', 'Z'};

/**
 * arm64_header_check_magic - Helper to check the arm64 image header.
 *
 * Returns non-zero if header is OK.
 */

static inline int arm64_header_check_magic(const struct arm64_image_header *h)
{
	if (!h)
		return 0;

	if (!h->text_offset)
		return 0;

	return (h->magic[0] == arm64_image_magic[0]
		&& h->magic[1] == arm64_image_magic[1]
		&& h->magic[2] == arm64_image_magic[2]
		&& h->magic[3] == arm64_image_magic[3]);
}

/**
 * arm64_header_check_pe_sig - Helper to check the arm64 image header.
 *
 * Returns non-zero if 'MZ' signature is found.
 */

static inline int arm64_header_check_pe_sig(const struct arm64_image_header *h)
{
	if (!h)
		return 0;

	return (h->pe_sig[0] == arm64_image_pe_sig[0]
		&& h->pe_sig[1] == arm64_image_pe_sig[1]);
}

extern const struct kexec_file_ops kexec_image_ops;

struct kimage;

#define arch_kimage_file_post_load_cleanup arch_kimage_file_post_load_cleanup
extern int arch_kimage_file_post_load_cleanup(struct kimage *image);

extern int load_other_segments(struct kimage *image,
		unsigned long kernel_load_addr,
		char *initrd, unsigned long initrd_len,
		char *cmdline, unsigned long cmdline_len);
extern int load_crashdump_segments(struct kimage *image);
#endif

#endif /* __ASSEMBLY__ */

#endif
