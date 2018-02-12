/* SPDX-License-Identifier: GPL-2.0 */
/*
 * genalloc-selftest.h
 *
 * (C) Copyright 2017 Huawei Technologies Co. Ltd.
 * Author: Igor Stoppa <igor.stoppa@huawei.com>
 */


#ifndef __LINUX_GENALLOC_SELFTEST_H
#define __LINUX_GENALLOC_SELFTEST_H


#ifdef CONFIG_GENERIC_ALLOCATOR_SELFTEST

#include <linux/genalloc.h>

void genalloc_selftest(void);

#else

static inline void genalloc_selftest(void){};

#endif

#endif
