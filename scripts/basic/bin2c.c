/*
 * Unloved program to convert a binary on stdin to a C include on stdout
 *
 * Jan 1999 Matt Mackall <mpm@selenic.com>
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 */

#include <stdio.h>
#include <errno.h>

int main(int argc, char *argv[])
{
	int ch, total = 0;

	if (argc > 1)
		if (printf("const char %s[] %s=\n",
			   argv[1], argc > 2 ? argv[2] : "") < 16)
			return errno;

	do {
		if (fputs("\t\"", stdout) < 0)
			return errno;

		while ((ch = getchar()) != EOF) {
			if (printf("\\x%02x", ch) < 4)
				return errno;
			if (++total % 16 == 0)
				break;
		}

		if (puts("\"") < 0)
			return errno;
	} while (ch != EOF);

	if (argc > 1)
		if (printf("\t;\n\n#include <linux/types.h>\n\nconst size_t %s_size = %d;\n",
			   argv[1], total) < 54)
			return errno;
	return 0;
}
