/*
 * Copyright (C) 2016 Red Hat
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 * Rob Clark <robdclark@gmail.com>
 */

#ifndef DRM_PRINT_H_
#define DRM_PRINT_H_

#include <linux/compiler.h>
#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/device.h>

/**
 * DOC: print
 *
 * A simple wrapper for dev_printk(), seq_printf(), etc.  Allows same
 * debug code to be used for both debugfs and printk logging.
 *
 * For example::
 *
 *     void log_some_info(struct drm_printer *p)
 *     {
 *             drm_printf(p, "foo=%d\n", foo);
 *             drm_printf(p, "bar=%d\n", bar);
 *     }
 *
 *     #ifdef CONFIG_DEBUG_FS
 *     void debugfs_show(struct seq_file *f)
 *     {
 *             struct drm_printer p = drm_seq_file_printer(f);
 *             log_some_info(&p);
 *     }
 *     #endif
 *
 *     void some_other_function(...)
 *     {
 *             struct drm_printer p = drm_info_printer(drm->dev);
 *             log_some_info(&p);
 *     }
 */

/**
 * struct drm_printer - drm output "stream"
 *
 * Do not use struct members directly.  Use drm_printer_seq_file(),
 * drm_printer_info(), etc to initialize.  And drm_printf() for output.
 */
struct drm_printer {
	/* private: */
	void (*printfn)(struct drm_printer *p, struct va_format *vaf);
	void *arg;
	const char *prefix;
};

void __drm_printfn_seq_file(struct drm_printer *p, struct va_format *vaf);
void __drm_printfn_info(struct drm_printer *p, struct va_format *vaf);
void __drm_printfn_debug(struct drm_printer *p, struct va_format *vaf);

__printf(2, 3)
void drm_printf(struct drm_printer *p, const char *f, ...);


/**
 * drm_seq_file_printer - construct a &drm_printer that outputs to &seq_file
 * @f:  the &struct seq_file to output to
 *
 * RETURNS:
 * The &drm_printer object
 */
static inline struct drm_printer drm_seq_file_printer(struct seq_file *f)
{
	struct drm_printer p = {
		.printfn = __drm_printfn_seq_file,
		.arg = f,
	};
	return p;
}

/**
 * drm_info_printer - construct a &drm_printer that outputs to dev_printk()
 * @dev: the &struct device pointer
 *
 * RETURNS:
 * The &drm_printer object
 */
static inline struct drm_printer drm_info_printer(struct device *dev)
{
	struct drm_printer p = {
		.printfn = __drm_printfn_info,
		.arg = dev,
	};
	return p;
}

/**
 * drm_debug_printer - construct a &drm_printer that outputs to pr_debug()
 * @prefix: debug output prefix
 *
 * RETURNS:
 * The &drm_printer object
 */
static inline struct drm_printer drm_debug_printer(const char *prefix)
{
	struct drm_printer p = {
		.printfn = __drm_printfn_debug,
		.prefix = prefix
	};
	return p;
}

__printf(6, 7)
void drm_dev_printk(const struct device *dev, const char *level,
		    unsigned int category, const char *function_name,
		    const char *prefix, const char *format, ...);
__printf(3, 4)
void drm_printk(const char *level, unsigned int category,
		const char *format, ...);

/* Macros to make printk easier */

#define _DRM_PRINTK(once, level, fmt, ...)				\
	do {								\
		printk##once(KERN_##level "[" DRM_NAME "] " fmt,	\
			     ##__VA_ARGS__);				\
	} while (0)

#define DRM_INFO(fmt, ...)						\
	_DRM_PRINTK(, INFO, fmt, ##__VA_ARGS__)
#define DRM_NOTE(fmt, ...)						\
	_DRM_PRINTK(, NOTICE, fmt, ##__VA_ARGS__)
#define DRM_WARN(fmt, ...)						\
	_DRM_PRINTK(, WARNING, fmt, ##__VA_ARGS__)

#define DRM_INFO_ONCE(fmt, ...)						\
	_DRM_PRINTK(_once, INFO, fmt, ##__VA_ARGS__)
#define DRM_NOTE_ONCE(fmt, ...)						\
	_DRM_PRINTK(_once, NOTICE, fmt, ##__VA_ARGS__)
#define DRM_WARN_ONCE(fmt, ...)						\
	_DRM_PRINTK(_once, WARNING, fmt, ##__VA_ARGS__)

/**
 * Error output.
 *
 * @dev: device pointer
 * @fmt: printf() like format string.
 */
#define DRM_DEV_ERROR(dev, fmt, ...)					\
	drm_dev_printk(dev, KERN_ERR, DRM_UT_NONE, __func__, " *ERROR*",\
		       fmt, ##__VA_ARGS__)
#define DRM_ERROR(fmt, ...)						\
	drm_printk(KERN_ERR, DRM_UT_NONE, fmt,	##__VA_ARGS__)

/**
 * Rate limited error output.  Like DRM_ERROR() but won't flood the log.
 *
 * @dev: device pointer
 * @fmt: printf() like format string.
 */
#define DRM_DEV_ERROR_RATELIMITED(dev, fmt, ...)			\
({									\
	static DEFINE_RATELIMIT_STATE(_rs,				\
				      DEFAULT_RATELIMIT_INTERVAL,	\
				      DEFAULT_RATELIMIT_BURST);		\
									\
	if (__ratelimit(&_rs))						\
		DRM_DEV_ERROR(dev, fmt, ##__VA_ARGS__);			\
})
#define DRM_ERROR_RATELIMITED(fmt, ...)					\
	DRM_DEV_ERROR_RATELIMITED(NULL, fmt, ##__VA_ARGS__)

#define DRM_DEV_INFO(dev, fmt, ...)					\
	drm_dev_printk(dev, KERN_INFO, DRM_UT_NONE, __func__, "", fmt,	\
		       ##__VA_ARGS__)

#define DRM_DEV_INFO_ONCE(dev, fmt, ...)				\
({									\
	static bool __print_once __read_mostly;				\
	if (!__print_once) {						\
		__print_once = true;					\
		DRM_DEV_INFO(dev, fmt, ##__VA_ARGS__);			\
	}								\
})

/**
 * Debug output.
 *
 * @dev: device pointer
 * @fmt: printf() like format string.
 */
#define DRM_DEV_DEBUG(dev, fmt, args...)				\
	drm_dev_printk(dev, KERN_DEBUG, DRM_UT_CORE, __func__, "", fmt,	\
		       ##args)
#define DRM_DEBUG(fmt, ...)						\
	drm_printk(KERN_DEBUG, DRM_UT_CORE, fmt, ##__VA_ARGS__)

#define DRM_DEV_DEBUG_DRIVER(dev, fmt, args...)				\
	drm_dev_printk(dev, KERN_DEBUG, DRM_UT_DRIVER, __func__, "",	\
		       fmt, ##args)
#define DRM_DEBUG_DRIVER(fmt, ...)					\
	drm_printk(KERN_DEBUG, DRM_UT_DRIVER, fmt, ##__VA_ARGS__)

#define DRM_DEV_DEBUG_KMS(dev, fmt, args...)				\
	drm_dev_printk(dev, KERN_DEBUG, DRM_UT_KMS, __func__, "", fmt,	\
		       ##args)
#define DRM_DEBUG_KMS(fmt, ...)					\
	drm_printk(KERN_DEBUG, DRM_UT_KMS, fmt, ##__VA_ARGS__)

#define DRM_DEV_DEBUG_PRIME(dev, fmt, args...)				\
	drm_dev_printk(dev, KERN_DEBUG, DRM_UT_PRIME, __func__, "",	\
		       fmt, ##args)
#define DRM_DEBUG_PRIME(fmt, ...)					\
	drm_printk(KERN_DEBUG, DRM_UT_PRIME, fmt, ##__VA_ARGS__)

#define DRM_DEV_DEBUG_ATOMIC(dev, fmt, args...)				\
	drm_dev_printk(dev, KERN_DEBUG, DRM_UT_ATOMIC, __func__, "",	\
		       fmt, ##args)
#define DRM_DEBUG_ATOMIC(fmt, ...)					\
	drm_printk(KERN_DEBUG, DRM_UT_ATOMIC, fmt, ##__VA_ARGS__)

#define DRM_DEV_DEBUG_VBL(dev, fmt, args...)				\
	drm_dev_printk(dev, KERN_DEBUG, DRM_UT_VBL, __func__, "", fmt,	\
		       ##args)
#define DRM_DEBUG_VBL(fmt, ...)					\
	drm_printk(KERN_DEBUG, DRM_UT_VBL, fmt, ##__VA_ARGS__)

#define _DRM_DEV_DEFINE_DEBUG_RATELIMITED(dev, level, fmt, args...)	\
({									\
	static DEFINE_RATELIMIT_STATE(_rs,				\
				      DEFAULT_RATELIMIT_INTERVAL,	\
				      DEFAULT_RATELIMIT_BURST);		\
	if (__ratelimit(&_rs))						\
		drm_dev_printk(dev, KERN_DEBUG, DRM_UT_ ## level,	\
			       __func__, "", fmt, ##args);		\
})

/**
 * Rate limited debug output. Like DRM_DEBUG() but won't flood the log.
 *
 * @dev: device pointer
 * @fmt: printf() like format string.
 */
#define DRM_DEV_DEBUG_RATELIMITED(dev, fmt, args...)			\
	DEV__DRM_DEFINE_DEBUG_RATELIMITED(dev, CORE, fmt, ##args)
#define DRM_DEBUG_RATELIMITED(fmt, args...)				\
	DRM_DEV_DEBUG_RATELIMITED(NULL, fmt, ##args)
#define DRM_DEV_DEBUG_DRIVER_RATELIMITED(dev, fmt, args...)		\
	_DRM_DEV_DEFINE_DEBUG_RATELIMITED(dev, DRIVER, fmt, ##args)
#define DRM_DEBUG_DRIVER_RATELIMITED(fmt, args...)			\
	DRM_DEV_DEBUG_DRIVER_RATELIMITED(NULL, fmt, ##args)
#define DRM_DEV_DEBUG_KMS_RATELIMITED(dev, fmt, args...)		\
	_DRM_DEV_DEFINE_DEBUG_RATELIMITED(dev, KMS, fmt, ##args)
#define DRM_DEBUG_KMS_RATELIMITED(fmt, args...)				\
	DRM_DEV_DEBUG_KMS_RATELIMITED(NULL, fmt, ##args)
#define DRM_DEV_DEBUG_PRIME_RATELIMITED(dev, fmt, args...)		\
	_DRM_DEV_DEFINE_DEBUG_RATELIMITED(dev, PRIME, fmt, ##args)
#define DRM_DEBUG_PRIME_RATELIMITED(fmt, args...)			\
	DRM_DEV_DEBUG_PRIME_RATELIMITED(NULL, fmt, ##args)

#endif /* DRM_PRINT_H_ */
