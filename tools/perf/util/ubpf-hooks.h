/*
 * Copyright (C) 2016, Wang Nan <wangnan0@huawei.com>
 * Copyright (C) 2016, He Kuang <hekuang@huawei.com>
 * Copyright (C) 2016, Huawei Inc.
 */
#ifndef __PERF_UBPF_HOOKS_H
#define __PERF_UBPF_HOOKS_H

#include <linux/compiler.h>

#define UBPF_HOOK_BREAKABLE	1

#define __UBPF_HOOK_PROTO(args...) args
#define __UBPF_HOOK_ARG(args...) args
#define __UBPF_HOOK_STRUCT__entry(args...) args
#define __field(type, item)		type	item;

#ifdef HAVE_UBPF_SUPPORT

#define __proto(args) args
#define UBPF_HOOK(name, proto, args, struct, assign) int ubpf_hook_##name(proto)

#include "ubpf-hooks-list.h"

#else

#define __proto(args) args __maybe_unused
#define UBPF_HOOK(name, proto, args, struct, assign)		\
	static inline int ubpf_hook_##name(proto) {return 0; }	\

#include "ubpf-hooks-list.h"
#endif

#undef UBPF_HOOK
#undef __UBPF_HOOK_PROTO
#undef __UBPF_HOOK_ARG
#undef __UBPF_HOOK_STRUCT__entry
#undef __field
#undef __proto

#endif /* __PERF_UBPF_HOOKS_H */
