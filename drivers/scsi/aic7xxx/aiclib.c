/*
 * Utility functions for AIC driver
 *
 * Copyright (c) 2017 Michał Mirosław
 *
 * License: GPLv2
 */

#include "aiclib.h"

void aic_printbuf_push(struct aic_dump_buffer *buf, const char *fmt, ...)
{
	va_list args;
	char *ptr;
	int len;

	ptr = buf->buf + buf->cur_col;

	va_start(args, fmt);
	len = vscnprintf(ptr, sizeof(buf->buf) - buf->cur_col, fmt, args);
	buf->cur_col += len;
	va_end(args);
}
EXPORT_SYMBOL_GPL(aic_printbuf_push);

void aic_printbuf_finish(struct aic_dump_buffer *buf)
{
	if (!buf->cur_col)
		return;

	printk("%s\n", buf->buf);
	buf->cur_col = 0;
}
EXPORT_SYMBOL_GPL(aic_printbuf_finish);

static void aic_printbuf_maybe_break(struct aic_dump_buffer *buf)
{
	if (buf->cur_col >= buf->wrap)
		aic_printbuf_finish(buf);
}

void aic_print_register(const aic_reg_parse_entry_t *table, u_int num_entries,
			const char *name, u_int address, u_int value,
			struct aic_dump_buffer *buf)
{
	u_int	printed_mask;

	aic_printbuf_push(buf, "%s[0x%x]", name, value);
	if (table == NULL) {
		aic_printbuf_push(buf, " ");
		aic_printbuf_maybe_break(buf);
		return;
	}

	printed_mask = 0;
	while (printed_mask != 0xFF) {
		int entry;

		for (entry = 0; entry < num_entries; entry++) {
			const aic_reg_parse_entry_t *e = &table[entry];
			if (((value & e->mask) != e->value) ||
			    ((printed_mask & e->mask) == e->mask))
				continue;

			aic_printbuf_push(buf, "%s%s",
					  printed_mask == 0 ? ":(" : "|",
					  e->name);
			printed_mask |= e->mask;

			break;
		}

		if (entry >= num_entries)
			break;
	}
	if (printed_mask != 0)
		aic_printbuf_push(buf, ") ");
	else
		aic_printbuf_push(buf, " ");

	aic_printbuf_maybe_break(buf);
}
EXPORT_SYMBOL_GPL(aic_print_register);
