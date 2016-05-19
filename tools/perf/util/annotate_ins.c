#ifndef HAVE_ANNOTATE_INS_SUPPORT

#include <linux/compiler.h>
#include <util/annotate_ins.h>

bool arch_is_return_ins(const char *s __maybe_unused)
{
	return false;
}

char *arch_parse_mov_comment(const char *s __maybe_unused)
{
	return NULL;
}

char *arch_parse_call_target(char *t)
{
	return t;
}

#endif /* !HAVE_ANNOTATE_INS_SUPPORT */
