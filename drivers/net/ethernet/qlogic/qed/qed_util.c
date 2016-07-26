#include <linux/kernel.h>
#include <linux/qed/qed_if.h>

#include "qed.h"

void qed_err(const char *name, int line, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	pr_err("[%ps:%d(%s)] %pV",
	       __builtin_return_address(0), line, name ? name : "",
               &vaf);

	va_end(args);
}

void qed_notice(u32 level, const char *name, int line, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (likely(level > QED_LEVEL_NOTICE))
		return;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	pr_notice("[%ps:%d(%s)] %pV",
		  __builtin_return_address(0), line, name ? name : "",
		  &vaf);

	va_end(args);
}

void qed_info(u32 level, const char *name, int line, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (likely(level > QED_LEVEL_INFO))
		return;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	pr_notice("[%ps:%d(%s)] %pV",
		  __builtin_return_address(0), line, name ? name : "",
		  &vaf);

	va_end(args);
}

void qed_verbose(u32 level, const char *name, int line,
		 const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (likely(level > QED_LEVEL_VERBOSE))
		return;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	pr_notice("[%ps:%d(%s)] %pV",
		  __builtin_return_address(0), line, name ? name : "",
		  &vaf);

	va_end(args);
}
