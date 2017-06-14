#include <string.h>
#include "../../util/fused.h"

bool fused_insn_pair(const char *insn1, const char *insn2)
{
	if (strstr(insn2, "jmp"))
		return false;

	if ((strstr(insn1, "cmp") && !strstr(insn1, "xchg")) ||
	    strstr(insn1, "test") ||
	    strstr(insn1, "add") ||
	    strstr(insn1, "sub") ||
	    strstr(insn1, "and") ||
	    strstr(insn1, "inc") ||
	    strstr(insn1, "dec")) {
		return true;
	}

	return false;
}
