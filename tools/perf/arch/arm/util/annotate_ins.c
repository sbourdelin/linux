#include <string.h>
#include <util/annotate_ins.h>

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
