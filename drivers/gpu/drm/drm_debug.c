/*
 * Copyright 2001 VA Linux Systems, Inc., Sunnyvale, California.
 * All Rights Reserved.
 *
 * Author Rickard E. (Rik) Faith <faith@valinux.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <drm/drm_debug.h>
#include <drm/drmP.h>

#define DRM_PRINTK_FMT "[" DRM_NAME ":%s]%s %pV"

void drm_dev_printk(const struct device *dev, const char *level,
		    unsigned int category, const char *function_name,
		    const char *prefix, const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	if (category != DRM_UT_NONE && !(drm_debug & category))
		return;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;

	if (dev)
		dev_printk(level, dev, DRM_PRINTK_FMT, function_name, prefix,
			   &vaf);
	else
		printk("%s" DRM_PRINTK_FMT, level, function_name, prefix, &vaf);

	va_end(args);
}
EXPORT_SYMBOL(drm_dev_printk);

void drm_printk(const char *level, unsigned int category,
		const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	if (category != DRM_UT_NONE && !(drm_debug & category))
		return;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;

	printk("%s" "[" DRM_NAME ":%ps]%s %pV",
	       level, __builtin_return_address(0),
	       strcmp(level, KERN_ERR) == 0 ? " *ERROR*" : "", &vaf);

	va_end(args);
}
EXPORT_SYMBOL(drm_printk);
