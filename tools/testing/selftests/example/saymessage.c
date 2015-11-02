#include <stdio.h>

int main(void)
{
	char buf[128];
	FILE *f;

	/*
	 * The selftests machinery runs the test in the directory it is built in,
	 * so you can find data files easily, they are in the current directory.
	 * To support installing the selftests you need to add any data files
	 * to TEST_FILES, they will then be copied into the install directory,
	 * where your tests will be run, so the C code doesn't need any extra
	 * logic.
	 */

	/*
	 * This test is written to work with or without message.txt, mainly
	 * just to demonstrate that it's possible. Your code should probably
	 * just always install any extra files.
	 */

#ifdef HAVE_MESSAGE_TXT
	f = fopen("message.txt", "r");
	if (!f) {
		perror("fopen");
		return 1;
	}

	if (!fgets(buf, sizeof(buf), f)) {
		perror("fgets");
		return 1;
	}
#else
	snprintf(buf, sizeof(buf), "builtin message\n");
#endif

	printf("Test message is: %s", buf);

	return 0;
}
