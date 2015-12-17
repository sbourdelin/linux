/*
 * This file is part of the Linux kernel, and is made available under
 * the terms of the GNU General Public License version 2.
 *
 * Misc librarized functions for cmdline poking.
 */
#include <linux/bug.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <asm/setup.h>

static inline int myisspace(u8 c)
{
	return c <= ' ';	/* Close enough approximation */
}

/**
 * Find a boolean option (like quiet,noapic,nosmp....)
 *
 * @cmdline: the cmdline string
 * @option: option string to look for
 *
 * Returns the position of that @option (starts counting with 1)
 * or 0 on not found.  @option will only be found if it is found
 * as an entire word in @cmdline.  For instance, if @option="car"
 * then a cmdline which contains "cart" will not match.
 */
static int __cmdline_find_option_bool(const char *cmdline,
		int max_cmdline_size, const char *option)
{
	char c;
	int pos = 0, wstart = 0;
	const char *opptr = NULL;
	enum {
		st_wordstart = 0,	/* Start of word/after whitespace */
		st_wordcmp,	/* Comparing this word */
		st_wordskip,	/* Miscompare, skip */
	} state = st_wordstart;

	if (!cmdline)
		return -1;      /* No command line */

	if (!strlen(cmdline))
		return 0;

	/*
	 * This 'pos' check ensures we do not overrun
	 * a non-NULL-terminated 'cmdline'
	 */
	while (pos < max_cmdline_size) {
		c = *(char *)cmdline++;
		pos++;

		switch (state) {
		case st_wordstart:
			if (!c)
				return 0;
			else if (myisspace(c))
				break;

			state = st_wordcmp;
			opptr = option;
			wstart = pos;
			/* fall through */

		case st_wordcmp:
			if (!*opptr) {
				/*
				 * We matched all the way to the end of the
				 * option we were looking for.  If the
				 * command-line has a space _or_ ends, then
				 * we matched!
				 */
				if (!c || myisspace(c))
					return wstart;
				else
					state = st_wordskip;
			} else if (!c) {
				/*
				 * Hit the NULL terminator on the end of
				 * cmdline.
				 */
				return 0;
			} else if (c != *opptr++) {
				state = st_wordskip;
			}
			break;

		case st_wordskip:
			if (!c)
				return 0;
			else if (myisspace(c))
				state = st_wordstart;
			break;
		}
	}

	return 0;	/* Buffer overrun */
}

int cmdline_find_option_bool(const char *cmdline, const char *option)
{
	return __cmdline_find_option_bool(cmdline, COMMAND_LINE_SIZE,
			option);
}

#ifdef CONFIG_X86_TEST_EARLY_CMDLINE

static int __cmdtest(char *cmdline, int str_size, char *option,
		int expected_result, int do_shrink)
{
	int ret;
	int null_terminate;
	/* Results are 1-based, so bias back down by 1 */
	int option_end = expected_result + strlen(option) - 1;
	int shrink_max = 0;

	if (cmdline && do_shrink)
		shrink_max = strlen(cmdline);
	/*
	 * The option was not found.  If it was not found in the
	 * *full* command-line, it should never be found in any
	 * *part* of the command-line.
	 */
	for (null_terminate = 0; null_terminate <= 1; null_terminate++) {
		int shrink_by;
		for (shrink_by = 0; shrink_by < shrink_max; shrink_by++) {
			int str_size_tst = str_size - shrink_by;
			char tmp = cmdline[str_size_tst];

			/*
			 * Do not run tests that would truncate
			 * over the expected option
			 */
			if (str_size_tst <= option_end)
				continue;

			if (null_terminate)
				cmdline[str_size_tst] = '\0';
			ret = __cmdline_find_option_bool(cmdline, str_size_tst,
					option);
			if (null_terminate)
				cmdline[str_size_tst] = tmp;

			if (ret == expected_result)
				continue;
			pr_err("failed cmdline test ('%s', %d, '%s') == %d "
					"nulld: %d got: %d\n",
					cmdline, str_size_tst, option,
					expected_result, null_terminate,
					ret);
			return 1;
		}
	}
	return 0;
}

#define cmdtest(cmdline, option, result)	\
	WARN_ON(__cmdtest(cmdline, sizeof(cmdline), option, result, 1))

#define cmdtest_noshrink(cmdline, option, result)	\
	WARN_ON(__cmdtest(cmdline, sizeof(cmdline), option, result, 0))

char cmdline1[] = "CALL me Ishmael  ";
char cmdline2[] = "Whenever I find myself growing grim about the mouth  ";
char cmdline3[] = "grow growing  ";
int test_early_cmdline(void)
{
	/* NULL command-line: */
	WARN_ON(__cmdline_find_option_bool(NULL, 22, "Ishmael") != -1);
	/* zero-length command-line: */
	cmdtest("", "Ishmael", 0);

	/* Find words at each of 3 positions: start, middle, end */
	cmdtest(cmdline1, "CALL", 1);
	cmdtest(cmdline1, "me", 6);
	cmdtest(cmdline1, "Ishmael", 9);

	/*
	 * Fail to find strings that all occur in the cmdline,
	 * but not as full words
	 */
	/*
	 * If "option" is _present_ in "cmdline" as the start of a
	 * word, like cmdline="foo bar" and we pass in option="b",
	 * when we shrink cmdline to "foo b", it will match.  So,
	 * skip shrink tests for those.
	 */
	cmdtest_noshrink(cmdline1, "m", 0);
	cmdtest(cmdline1, "e", 0);
	cmdtest(cmdline1, "C", 0);
	cmdtest(cmdline1, "l", 0);
	cmdtest_noshrink(cmdline1, "Ishmae", 0);
	cmdtest(cmdline1, "mael", 0);
	/*
	 * Look for strings that do not occur, but match until
	 * close to the end of cmdline
	 */
	cmdtest_noshrink(cmdline1, "Ishmae", 0);
	cmdtest(cmdline1, "Ishmaels", 0);
	cmdtest(cmdline1, "maels", 0);

	/*
	 * Look for full words that do not occur in a different
	 * cmdline
	 */
	cmdtest(cmdline2, "CALL", 0);
	cmdtest(cmdline2, "me", 0);
	cmdtest(cmdline2, "Ishmael", 0);
	/*
	 * Look for full words which do occur in cmdline2
	 */
	cmdtest(cmdline2, "Whenever", 1);
	cmdtest(cmdline2, "growing", 24);
	cmdtest(cmdline2, "grim", 32);
	cmdtest(cmdline2, "mouth", 47);

	/*
	 * Catch the bug where if we match a partial word and
	 * then have a space, we do not match the _next_ word.
	 */
	cmdtest(cmdline3, "grow", 1);
	cmdtest(cmdline3, "growing", 6);
	return 0;
}
#endif /* CONFIG_X86_TEST_EARLY_CMDLINE */
