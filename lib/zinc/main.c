/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2015-2018 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <zinc/chacha20.h>

#include <linux/init.h>
#include <linux/module.h>

#ifdef DEBUG
#define selftest(which) do { \
	if (!which ## _selftest()) \
		return -ENOTRECOVERABLE; \
} while (0)
#else
#define selftest(which)
#endif

static int __init mod_init(void)
{
#ifdef CONFIG_ZINC_CHACHA20
	chacha20_fpu_init();
#endif
	return 0;
}

static void __exit mod_exit(void)
{
}

module_init(mod_init);
module_exit(mod_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Zinc cryptography library");
MODULE_AUTHOR("Jason A. Donenfeld <Jason@zx2c4.com>");
