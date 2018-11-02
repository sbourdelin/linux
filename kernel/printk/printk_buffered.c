/* SPDX-License-Identifier: GPL-2.0+ */

#include <linux/types.h> /* DECLARE_BITMAP() */
#include <linux/printk.h>
#include "internal.h"

/* A structure for line-buffered printk() output. */
struct printk_buffer {
	unsigned short int len; /* Valid bytes in buf[]. */
	char buf[LOG_LINE_MAX];
} __aligned(1024);

/*
 * Number of statically preallocated buffers.
 *
 * We can introduce a kernel config option if someone wants to tune this value.
 * But since "struct printk_buffer" makes difference only when there are
 * multiple threads concurrently calling printk() which does not end with '\n',
 * and this API will fallback to normal printk() when all buffers are in use,
 * it is possible that nobody needs to tune this value.
 */
#define NUM_LINE_BUFFERS 16

static struct printk_buffer printk_buffers[NUM_LINE_BUFFERS];
static DECLARE_BITMAP(printk_buffers_in_use, NUM_LINE_BUFFERS);

/**
 * get_printk_buffer - Try to get printk_buffer.
 *
 * Returns pointer to "struct printk_buffer" on success, NULL otherwise.
 *
 * If this function returned "struct printk_buffer", the caller is responsible
 * for passing it to put_printk_buffer() so that "struct printk_buffer" can be
 * reused in the future.
 *
 * Even if this function returned NULL, the caller does not need to check for
 * NULL, for passing NULL to printk_buffered() simply acts like normal printk()
 * and passing NULL to flush_printk_buffer()/put_printk_buffer() is a no-op.
 */
struct printk_buffer *get_printk_buffer(void)
{
	long i;

	for (i = 0; i < NUM_LINE_BUFFERS; i++) {
		if (test_and_set_bit_lock(i, printk_buffers_in_use))
			continue;
		printk_buffers[i].len = 0;
		return &printk_buffers[i];
	}
	return NULL;
}
EXPORT_SYMBOL(get_printk_buffer);

/**
 * vprintk_buffered - Try to vprintk() in line buffered mode.
 *
 * @ptr:  Pointer to "struct printk_buffer". It can be NULL.
 * @fmt:  printk() format string.
 * @args: va_list structure.
 *
 * Returns the return value of vprintk().
 *
 * Try to store to @ptr first. If it fails, flush @ptr and then try to store to
 * @ptr again. If it still fails, use unbuffered printing.
 */
int vprintk_buffered(struct printk_buffer *ptr, const char *fmt, va_list args)
{
	va_list tmp_args;
	int r;
	int pos;

	if (!ptr)
		return vprintk(fmt, args);
	/*
	 * Skip KERN_CONT here based on an assumption that KERN_CONT will be
	 * given via "fmt" argument when KERN_CONT is given.
	 */
	pos = (printk_get_level(fmt) == 'c') ? 2 : 0;
	while (true) {
		va_copy(tmp_args, args);
		r = vsnprintf(ptr->buf + ptr->len, sizeof(ptr->buf) - ptr->len,
			      fmt + pos, tmp_args);
		va_end(tmp_args);
		if (likely(r + ptr->len < sizeof(ptr->buf)))
			break;
		if (!flush_printk_buffer(ptr))
			return vprintk(fmt, args);
	}
	ptr->len += r;
	/* Flush already completed lines if any. */
	for (pos = ptr->len - 1; pos >= 0; pos--) {
		if (ptr->buf[pos] != '\n')
			continue;
		ptr->buf[pos++] = '\0';
		printk("%s\n", ptr->buf);
		ptr->len -= pos;
		memmove(ptr->buf, ptr->buf + pos, ptr->len);
		/* This '\0' will be overwritten by next vsnprintf() above. */
		ptr->buf[ptr->len] = '\0';
		break;
	}
	return r;
}

/**
 * printk_buffered - Try to printk() in line buffered mode.
 *
 * @ptr: Pointer to "struct printk_buffer". It can be NULL.
 * @fmt: printk() format string, followed by arguments.
 *
 * Returns the return value of printk().
 *
 * Try to store to @ptr first. If it fails, flush @ptr and then try to store to
 * @ptr again. If it still fails, use unbuffered printing.
 */
int printk_buffered(struct printk_buffer *ptr, const char *fmt, ...)
{
	va_list args;
	int r;

	va_start(args, fmt);
	r = vprintk_buffered(ptr, fmt, args);
	va_end(args);
	return r;
}
EXPORT_SYMBOL(printk_buffered);

/**
 * flush_printk_buffer - Flush incomplete line in printk_buffer.
 *
 * @ptr: Pointer to "struct printk_buffer". It can be NULL.
 *
 * Returns true if flushed something, false otherwise.
 *
 * Flush if @ptr contains partial data. But usually there is no need to call
 * this function because @ptr is flushed by put_printk_buffer().
 */
bool flush_printk_buffer(struct printk_buffer *ptr)
{
	if (!ptr || !ptr->len)
		return false;
	/*
	 * vprintk_buffered() keeps 0 <= ptr->len < sizeof(ptr->buf) true.
	 * But ptr->buf[ptr->len] != '\0' if this function is called due to
	 * vsnprintf() + ptr->len >= sizeof(ptr->buf).
	 */
	ptr->buf[ptr->len] = '\0';
	printk("%s", ptr->buf);
	ptr->len = 0;
	return true;
}
EXPORT_SYMBOL(flush_printk_buffer);

/**
 * __put_printk_buffer - Release printk_buffer.
 *
 * @ptr: Pointer to "struct printk_buffer". It can be NULL.
 *
 * Returns nothing.
 *
 * Flush and release @ptr.
 * Please use put_printk_buffer() in order to catch use-after-free bugs.
 */
void __put_printk_buffer(struct printk_buffer *ptr)
{
	long i = (unsigned long) ptr - (unsigned long) printk_buffers;

	if (!ptr)
		return;
	if (WARN_ON_ONCE(i % sizeof(struct printk_buffer)))
		return;
	i /= sizeof(struct printk_buffer);
	if (WARN_ON_ONCE(i < 0 || i >= NUM_LINE_BUFFERS))
		return;
	if (ptr->len)
		flush_printk_buffer(ptr);
	clear_bit_unlock(i, printk_buffers_in_use);
}
EXPORT_SYMBOL(__put_printk_buffer);
