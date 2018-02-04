/*
 * genalloc-selftest.h
 *
 * (C) Copyright 2017 Huawei Technologies Co. Ltd.
 * Author: Igor Stoppa <igor.stoppa@huawei.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */


#ifndef __GENALLOC_SELFTEST_H__
#define __GENALLOC_SELFTEST_H__


#ifdef CONFIG_GENERIC_ALLOCATOR_SELFTEST

#include <linux/genalloc.h>

void genalloc_selftest(void);

#else

static inline void genalloc_selftest(void){};

#endif

#endif
