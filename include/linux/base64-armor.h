#ifndef __LINUX_BASE64_ARMOR_H__
#define __LINUX_BASE64_ARMOR_H__

#include <linux/types.h>

/**
 * base64_armor: Perform armored base64 encoding. Output may or may
 * not contain newlines, depending on input length.
 *
 * @dst: Beginning of the destination buffer.
 * @dst_max: Maximum amount of bytes to write to the destination buffer.
 * @src: Beginning of the source buffer.
 * @end: Sentinel for the source buffer, pointing one byte after the
 *       last byte to be encoded.
 *
 * Returns the number of bytes written to the destination buffer, or
 * an error of the output buffer is insufficient in size.
 *
 * _Neither_ the input or output are expected to be NULL-terminated.
 *
 * The number of output bytes is exactly (n * 4 + (n / 16)) where
 * n = ((end - src) + 2) / 3. A less stringent but more wasteful
 * validation for output buffer size can be: 4 + (end - src) * 2.
 *
 * See base64_encode_buffer_bound below.
 */
extern int base64_armor(char *dst, int dst_max, const char *src,
			const char *end);

/**
 * base64_unarmor: Perform armored base64 decoding.
 *
 * @dst: Beginning of the destination buffer.
 * @dst_max: Maximum amount of bytes to write to the destination buffer.
 * @src: Beginning of the source buffer
 * @end: Sentinel for the source buffer, pointing one byte after the
 *       last byte to be encoded.
 *
 * Returns the number of bytes written to the destination buffer,
 * -EINVAL if the source buffer contains invalid bytes, or -ENOSPC
 * if the output buffer is insufficient in size.
 *
 * _Neither_ the input or output are expected to be NULL-terminated.
 *
 * It can be assumed that the number of output bytes is less or
 * equals to: 3 * ((end - src) / 4).
 *
 * See base64_decode_buffer_bound below.
 */
extern int base64_unarmor(char *dst, int dst_max, const char *src,
			  const char *end);


/*
 * Utility functions for buffer upper bounds:
 */

static inline size_t base64_encode_buffer_bound(size_t src_len)
{
	size_t n = (src_len + 2) / 3;

	return (n * 4 + (n / 16));
}

static inline size_t base64_decode_buffer_bound(size_t src_len)
{
	return 3 * (src_len / 4);
}

#endif
