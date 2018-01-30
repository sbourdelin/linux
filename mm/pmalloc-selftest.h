/*
 * pmalloc-selftest.h
 *
 * (C) Copyright 2018 Huawei Technologies Co. Ltd.
 * Author: Igor Stoppa <igor.stoppa@huawei.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */


#ifndef __PMALLOC_SELFTEST_H__
#define __PMALLOC_SELFTEST_H__


#ifdef CONFIG_PROTECTABLE_MEMORY_SELFTEST

#include <linux/pmalloc.h>

void pmalloc_selftest(void);

#else

static inline void pmalloc_selftest(void){};

#endif

#endif
