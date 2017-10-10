/*
 * kexec core purgatory
 */
#include <linux/linkage.h>
#include <uapi/linux/kexec.h>

#define SHA256_DIGEST_SIZE	32 /* defined in crypto/sha.h */

.text

ENTRY(purgatory_start)
	/* Start new image. */
	ldr	x17, __kernel_entry
	ldr	x0, __dtb_addr
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x17
END(purgatory_start)

/*
 * data section:
 * kernel_entry and dtb_addr are global but also labelled as local,
 * "__xxx:", to avoid unwanted re-linking.
 *
 * purgatory_sha_regions and purgatory_sha256_digest are referenced
 * by kexec generic code and so must exist, but not actually used
 * here because hash check is not that useful in purgatory.
 */
.align 3

.globl kernel_entry
kernel_entry:
__kernel_entry:
	.quad	0
END(kernel_entry)

.globl dtb_addr
dtb_addr:
__dtb_addr:
	.quad	0
END(dtb_addr)

.globl purgatory_sha_regions
purgatory_sha_regions:
	.rept	KEXEC_SEGMENT_MAX
	.quad	0
	.quad	0
	.endr
END(purgatory_sha_regions)

.globl purgatory_sha256_digest
purgatory_sha256_digest:
        .skip   SHA256_DIGEST_SIZE
END(purgatory_sha256_digest)
