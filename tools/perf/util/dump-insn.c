#include "perf.h"
#include "dump-insn.h"

/* Fallback code */

__weak
const char *dump_insn(struct perf_insn *x __maybe_unused,
		      uint64_t ip __maybe_unused,
		      u8 *inbuf __maybe_unused,
		      int inlen __maybe_unused,
		      int *lenp __maybe_unused)
{
	if (lenp)
		*lenp = 0;
	return "?";
}
