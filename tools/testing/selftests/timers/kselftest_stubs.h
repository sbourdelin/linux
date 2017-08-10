/*
 * kselftest_stubs.h:	kselftest framework stubs
 *
 * Copyright (c) 2017 Shuah Khan <shuahkh@osg.samsung.com>
 * Copyright (c) 2017 Samsung Electronics Co., Ltd.
 *
 * This file is released under the GPLv2.
 *
 * This is stub file for ksft_* API to continue to build timer tests
 * without Kselftest framework.
 */
#ifndef __KSELFTEST_STUBS_H
#define __KSELFTEST_STUBS_H

#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>

static inline int ksft_exit_pass(void) { exit(0); }
static inline int ksft_exit_fail(void) { exit(1); }
static inline int ksft_exit_skip(const char *msg, ...) { exit(4); }

#endif /* __KSELFTEST__STUBS_H */
