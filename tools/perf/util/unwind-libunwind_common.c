#include <elf.h>
#include <gelf.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/list.h>
#include <libunwind.h>
#include <libunwind-ptrace.h>
#include "callchain.h"
#include "thread.h"
#include "session.h"
#include "perf_regs.h"
#include "unwind.h"
#include "symbol.h"
#include "util.h"
#include "debug.h"
#include "asm/bug.h"

static int __null__prepare_access(struct thread *thread __maybe_unused)
{
	return 0;
}

static void __null__flush_access(struct thread *thread __maybe_unused)
{
}

static void __null__finish_access(struct thread *thread __maybe_unused)
{
}

static int __null__get_entries(unwind_entry_cb_t cb __maybe_unused,
			       void *arg __maybe_unused,
			       struct thread *thread __maybe_unused,
			       struct perf_sample *data __maybe_unused,
			       int max_stack __maybe_unused)
{
	return 0;
}

static struct unwind_libunwind_ops null_unwind_libunwind_ops = {
	.prepare_access = __null__prepare_access,
	.flush_access   = __null__flush_access,
	.finish_access  = __null__finish_access,
	.get_entries    = __null__get_entries,
};

int register_null_unwind_libunwind_ops(struct thread *thread)
{
	thread->unwind_libunwind_ops = &null_unwind_libunwind_ops;
	return 0;
}

int register_unwind_libunwind_ops(struct unwind_libunwind_ops *ops,
				  struct thread *thread)
{
	thread->unwind_libunwind_ops = ops;
	return 0;
}
