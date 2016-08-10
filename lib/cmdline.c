/*
 * linux/lib/cmdline.c
 * Helper functions generally used for parsing kernel command line
 * and module options.
 *
 * Code and copyrights come from init/main.c and arch/i386/kernel/setup.c.
 *
 * This source code is licensed under the GNU General Public License,
 * Version 2.  See the file COPYING for more details.
 *
 * GNU Indent formatting options for this file: -kr -i8 -npsl -pcs
 *
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/string.h>

/*
 *	If a hyphen was found in get_option, this will handle the
 *	range of numbers, M-N.  This will expand the range and insert
 *	the values[M, M+1, ..., N] into the ints array in get_options.
 */

static int get_range(char **str, int *pint)
{
	int x, inc_counter, upper_range;

	(*str)++;
	upper_range = simple_strtol((*str), NULL, 0);
	inc_counter = upper_range - *pint;
	for (x = *pint; x < upper_range; x++)
		*pint++ = x;
	return inc_counter;
}

/**
 *	get_option - Parse integer from an option string
 *	@str: option string
 *	@pint: (output) integer value parsed from @str
 *
 *	Read an int from an option string; if available accept a subsequent
 *	comma as well.
 *
 *	Return values:
 *	0 - no int in string
 *	1 - int found, no subsequent comma
 *	2 - int found including a subsequent comma
 *	3 - hyphen found to denote a range
 */

int get_option(char **str, int *pint)
{
	char *cur = *str;

	if (!cur || !(*cur))
		return 0;
	*pint = simple_strtol(cur, str, 0);
	if (cur == *str)
		return 0;
	if (**str == ',') {
		(*str)++;
		return 2;
	}
	if (**str == '-')
		return 3;

	return 1;
}
EXPORT_SYMBOL(get_option);

/**
 *	get_options - Parse a string into a list of integers
 *	@str: String to be parsed
 *	@nints: size of integer array
 *	@ints: integer array
 *
 *	This function parses a string containing a comma-separated
 *	list of integers, a hyphen-separated range of _positive_ integers,
 *	or a combination of both.  The parse halts when the array is
 *	full, or when no more numbers can be retrieved from the
 *	string.
 *
 *	Return value is the character in the string which caused
 *	the parse to end (typically a null terminator, if @str is
 *	completely parseable).
 */

char *get_options(const char *str, int nints, int *ints)
{
	int res, i = 1;

	while (i < nints) {
		res = get_option((char **)&str, ints + i);
		if (res == 0)
			break;
		if (res == 3) {
			int range_nums;
			range_nums = get_range((char **)&str, ints + i);
			if (range_nums < 0)
				break;
			/*
			 * Decrement the result by one to leave out the
			 * last number in the range.  The next iteration
			 * will handle the upper number in the range
			 */
			i += (range_nums - 1);
		}
		i++;
		if (res == 1)
			break;
	}
	ints[0] = i - 1;
	return (char *)str;
}
EXPORT_SYMBOL(get_options);

/**
 *	memparse - parse a string with mem suffixes into a number
 *	@ptr: Where parse begins
 *	@retptr: (output) Optional pointer to next char after parse completes
 *
 *	Parses a string into a number.  The number stored at @ptr is
 *	potentially suffixed with K, M, G, T, P, E.
 */

unsigned long long memparse(const char *ptr, char **retptr)
{
	char *endptr;	/* local pointer to end of parsed string */

	unsigned long long ret = simple_strtoull(ptr, &endptr, 0);

	switch (*endptr) {
	case 'E':
	case 'e':
		ret <<= 10;
	case 'P':
	case 'p':
		ret <<= 10;
	case 'T':
	case 't':
		ret <<= 10;
	case 'G':
	case 'g':
		ret <<= 10;
	case 'M':
	case 'm':
		ret <<= 10;
	case 'K':
	case 'k':
		ret <<= 10;
		endptr++;
	default:
		break;
	}

	if (retptr)
		*retptr = endptr;

	return ret;
}
EXPORT_SYMBOL(memparse);

/**
 *	parse_option_str - Parse a string and check an option is set or not
 *	@str: String to be parsed
 *	@option: option name
 *
 *	This function parses a string containing a comma-separated list of
 *	strings like a=b,c.
 *
 *	Return true if there's such option in the string, or return false.
 */
bool parse_option_str(const char *str, const char *option)
{
	while (*str) {
		if (!strncmp(str, option, strlen(option))) {
			str += strlen(option);
			if (!*str || *str == ',')
				return true;
		}

		while (*str && *str != ',')
			str++;

		if (*str == ',')
			str++;
	}

	return false;
}

/*
 * is_colon_in_param - check if current parameter has colon in it
 * @cmdline: points to the parameter to check
 *
 * This function checks if the current parameter has a colon in it.
 * Can be used for parameters that are range based or likewise.
 *
 * Returns true when the current paramer is has colon, false otherwise
 */
bool __init is_colon_in_param(const char *cmdline)
{
	char    *first_colon, *first_space;

	first_colon = strchr(cmdline, ':');
	first_space = strchr(cmdline, ' ');
	if (first_colon && (!first_space || first_colon < first_space))
		return true;

	return false;
}

/*
 * parse_mem_range_size - parse size based on memory range
 * @param:  the parameter being parsed
 * @str: (input)  where parsing begins
 *                expected format - <range1>:<size1>[,<range2>:<size2>,...]
 *                Range is specified with a number (potentially suffixed with
 *                K, M, G, T, P, E), signifying an inclusive start size,
 *                followed by a hypen and possibly another number signifying
 *                an exclusive end size
 *                Eg. : 4G-8G:512M,8G-32G:1024M,32G-128G:2048M,128G-:4096M
 *       (output) On success - next char after parse completes
 *                On failure - unchanged
 * @system_ram: system ram size to check memory range against
 *
 * Returns the memory size on success and 0 on failure
 */
unsigned long long __init parse_mem_range_size(const char *param,
					       char **str,
					       unsigned long long system_ram)
{
	char *cur = *str, *tmp;
	unsigned long long mem_size = 0;

	/* for each entry of the comma-separated list */
	do {
		unsigned long long start, end = ULLONG_MAX, size;

		/* get the start of the range */
		start = memparse(cur, &tmp);
		if (cur == tmp) {
			printk(KERN_INFO "%s: memory value expected\n", param);
			return mem_size;
		}
		cur = tmp;
		if (*cur != '-') {
			printk(KERN_INFO "%s: '-' expected\n", param);
			return mem_size;
		}
		cur++;

		/* if no ':' is here, than we read the end */
		if (*cur != ':') {
			end = memparse(cur, &tmp);
			if (cur == tmp) {
				printk(KERN_INFO "%s: memory value expected\n",
					param);
				return mem_size;
			}
			cur = tmp;
			if (end <= start) {
				printk(KERN_INFO "%s: end <= start\n", param);
				return mem_size;
			}
		}

		if (*cur != ':') {
			printk(KERN_INFO "%s: ':' expected\n", param);
			return mem_size;
		}
		cur++;

		size = memparse(cur, &tmp);
		if (cur == tmp) {
			printk(KERN_INFO "%s: memory value expected\n", param);
			return mem_size;
		}
		cur = tmp;
		if (size >= system_ram) {
			printk(KERN_INFO "%s: invalid size\n", param);
			return mem_size;
		}

		/* match ? */
		if (system_ram >= start && system_ram < end) {
			mem_size = size;
			*str = cur;
			break;
		}
	} while (*cur++ == ',');

	return mem_size;
}
