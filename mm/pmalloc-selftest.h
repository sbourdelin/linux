/* SPDX-License-Identifier: GPL-2.0
 *
 * pmalloc-selftest.h
 *
 * (C) Copyright 2018 Huawei Technologies Co. Ltd.
 * Author: Igor Stoppa <igor.stoppa@huawei.com>
 */


#ifndef __MM_PMALLOC_SELFTEST_H
#define __MM_PMALLOC_SELFTEST_H


#ifdef CONFIG_PROTECTABLE_MEMORY_SELFTEST

void pmalloc_selftest(void);

#else

static inline void pmalloc_selftest(void){};

#endif

#endif
