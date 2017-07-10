/*
 * pmalloc_usercopy.h: Pmalloc integrtion with Hardened Usercopy
 *
 * (C) Copyright 2017 Huawei Technologies Co. Ltd.
 * Author: Igor Stoppa <igor.stoppa@huawei.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#ifndef _PMALLOC_USERCOPY_H
#define _PMALLOC_USERCOPY_H

/**
 * is_pmalloc_page - check if a memory page is a pmalloc one.
 * @page: the page to be tested
 *
 * Tests a page for belonging to a pmalloc pool.
 * Returns a boolean result.
 */
bool is_pmalloc_page(struct page *page);


/**
 * pmalloc_check_range - verify that a range belongs to pmalloc
 * @ptr: starting point of the range to verify
 * @n: amount of bytes to consider
 *
 * Checks for all pages in the given range to be of pmalloc type.
 *
 * Returns NULL if the test is successful, otherwise a pointer
 * to an error string.
 */
void *pmalloc_check_range(const void *ptr, unsigned long n);

#endif
