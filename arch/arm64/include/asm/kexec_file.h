#ifndef _ASM_KEXEC_FILE_H
#define _ASM_KEXEC_FILE_H

extern struct kexec_file_ops kexec_image_ops;

/**
 * struct arm64_image_header - arm64 kernel image header.
 *
 * @pe_sig: Optional PE format 'MZ' signature.
 * @branch_code: Reserved for instructions to branch to stext.
 * @text_offset: The image load offset in LSB byte order.
 * @image_size: An estimated size of the memory image size in LSB byte order.
 * @flags: Bit flags:
 *  Bit 7.0: Image byte order, 1=MSB.
 * @reserved_1: Reserved.
 * @magic: Magic number, "ARM\x64".
 * @pe_header: Optional offset to a PE format header.
 **/

struct arm64_image_header {
	u8 pe_sig[2];
	u16 branch_code[3];
	u64 text_offset;
	u64 image_size;
	u8 flags[8];
	u64 reserved_1[3];
	u8 magic[4];
	u32 pe_header;
};

static const u8 arm64_image_magic[4] = {'A', 'R', 'M', 0x64U};
static const u8 arm64_image_pe_sig[2] = {'M', 'Z'};
static const u64 arm64_image_flag_7_be = 0x01U;

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
#endif /* _ASM_KEXEC_FILE_H */
