/*
 * Copyright (C) 2016, Wang Nan <wangnan0@huawei.com>
 * Copyright (C) 2016, He Kuang <hekuang@huawei.com>
 * Copyright (C) 2016, Huawei Inc.
 */

#include <bpf/libbpf.h>
#include <asm/bug.h>
#include <ubpf.h>
#include "ubpf-hooks.h"
#include "debug.h"

static int run_ubpf_program(struct bpf_program *prog __maybe_unused,
			    void *mem __maybe_unused, size_t len __maybe_unused)
{
	return 0;
}

static int
run_ubpf_programs(const char *expect_title, void *mem, size_t len)
{

	struct bpf_object *obj, *tmp;
	struct bpf_program *prog;
	const char *title;
	int err;

	bpf_object__for_each_safe(obj, tmp) {
		bpf_object__for_each_program(prog, obj) {
			if (bpf_program__is_ubpf(prog)) {
				title = bpf_program__title(prog, false);
				if (!title)
					continue;
				if (strcmp(title, expect_title) != 0)
					continue;
				err = run_ubpf_program(prog, mem, len);
				if (err)
					return err;
			}
		}
	}

	return 0;
}

#define __UBPF_HOOK_PROTO(args...) args
#define __UBPF_HOOK_ARG(args...) args
#define __UBPF_HOOK_STRUCT__entry(args...) args
#define __proto(args) args
#define __field(type, item)		type	item;

#define UBPF_HOOK(name, proto, args, tstruct, assign)	\
	struct ubpf_hook_##name##_proto {tstruct}
#include "ubpf-hooks-list.h"
#undef UBPF_HOOK

#define __UBPF_HOOK_ASSIGN(code...) code

#define UBPF_HOOK(name, proto, args, tstruct, assign)			\
	int ubpf_hook_##name(proto)					\
	{								\
		struct ubpf_hook_##name##_proto __entry;		\
		assign;							\
		return run_ubpf_programs("UBPF;"#name,			\
					 &__entry, sizeof(__entry));	\
	}
#include "ubpf-hooks-list.h"
#undef UBPF_HOOK
