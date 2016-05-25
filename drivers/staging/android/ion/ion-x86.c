/*
 * Copyright (C) 2016 Laura Abbott <laura@labbott.name>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/types.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>

#include "ion_priv.h"

void ion_clean_page(struct page *page, size_t size)
{
	/* Do nothing right now */
}

void ion_invalidate_buffer(struct ion_buffer *buffer)
{
	/* Do nothing right now */
}

void ion_clean_buffer(struct ion_buffer *buffer)
{
	/* Do nothing right now */
}
