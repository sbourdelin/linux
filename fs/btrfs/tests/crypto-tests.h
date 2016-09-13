/*
 * Copyright (C) 2016 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#define BTRFS_CONFIG_TEST_ABLKCIPHER	1
#define BTRFS_CONFIG_ZLIB_AS_ENCRYPT	1
#define BTRFS_CONFIG_COMP_INT		1
#define BTRFS_TEST_KEY			0

//#define TEST_DATA_SIZE	16
//#define TEST_DATA_SIZE	PAGE_CACHE_SIZE
//#define TEST_DATA_SIZE	1024
#define TEST_DATA_SIZE		2024

void test_ablkcipher(void);
void test_blkcipher(void);
void test_print_data(const char *str, char *prefix, size_t sz, int print_str);
void test_key(char *keytag);
void test_pr_result(struct page *page_in, int for_encrypt);
struct page *test_alloc_page_cpy_known_data(void);
void test_fini(void);
void test_open_key(void);
void test_close_key(void);
void btrfs_selftest_crypto(void);
