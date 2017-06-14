#include <linux/compiler.h>
#include <linux/types.h>
#include <string.h>

#include "fused.h"

bool __weak fused_insn_pair(const char *insn1 __maybe_unused,
			    const char *insn2 __maybe_unused)
{
	return false;
}
