#include <string.h>
#include <util/annotate_ins.h>

#include <linux/compiler.h>

bool arch_is_return_ins(const char *s __maybe_unused)
{
	return false;
}

char *arch_parse_mov_comment(const char *s)
{
	return strchr(s, ';');
}

char *arch_parse_call_target(char *t)
{
	if (strchr(t, '+'))
		return NULL;

	return t;
}
