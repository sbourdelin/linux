/*
 * pmalloc.h: Protectable Memory Allocator local header
 *
 * (C) Copyright 2017 Huawei Technologies Co. Ltd.
 * Author: Igor Stoppa <igor.stoppa@huawei.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */


#ifndef __PMALLOC_H
#define __PMALLOC_H
const char *__pmalloc_check_object(const void *ptr, unsigned long n);
#endif
