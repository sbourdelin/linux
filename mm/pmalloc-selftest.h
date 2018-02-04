/* SPDX-License-Identifier: GPL-2.0
 *
 * pmalloc-selftest.h
 *
 * (C) Copyright 2018 Huawei Technologies Co. Ltd.
 * Author: Igor Stoppa <igor.stoppa@huawei.com>
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
