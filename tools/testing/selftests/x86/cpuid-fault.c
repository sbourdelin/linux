
/*
 * Tests for arch_prctl(ARCH_GET_CPUID, ...) / arch_prctl(ARCH_SET_CPUID, ...)
 *
 * Basic test to test behaviour of ARCH_GET_CPUID and ARCH_SET_CPUID
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <inttypes.h>
#include <cpuid.h>
#include <err.h>
#include <errno.h>
#include <sys/wait.h>

#include <sys/prctl.h>
#include <linux/prctl.h>

/*
#define ARCH_GET_CPUID 0x1005
#define ARCH_SET_CPUID 0x1006
#ifdef __x86_64__
#define SYS_arch_prctl 158
#else
#define SYS_arch_prctl 385
#endif
*/

const char *cpuid_names[] = {
	[0] = "[cpuid disabled]",
	[1] = "[cpuid enabled]",
};

int arch_prctl(int code, unsigned long arg2)
{
	return syscall(SYS_arch_prctl, code, arg2);
}

int cpuid(unsigned int *eax, unsigned int *ebx, unsigned int *ecx,
	  unsigned int *edx)
{
	return __get_cpuid(0, eax, ebx, ecx, edx);
}

int do_child_exec_test(int eax, int ebx, int ecx, int edx)
{
	int cpuid_val = 0, child = 0, status = 0;

	printf("arch_prctl(ARCH_GET_CPUID); ");

	cpuid_val = arch_prctl(ARCH_GET_CPUID, 0);
	if (cpuid_val < 0)
		errx(1, "ARCH_GET_CPUID fails now, but not before?");

	printf("cpuid_val == %s\n", cpuid_names[cpuid_val]);
	if (cpuid_val != 0)
		errx(1, "How did cpuid get re-enabled on fork?");

	child = fork();
	if (child == 0) {
		cpuid_val = arch_prctl(ARCH_GET_CPUID, 0);
		if (cpuid_val < 0)
			errx(1, "ARCH_GET_CPUID fails now, but not before?");

		printf("cpuid_val == %s\n", cpuid_names[cpuid_val]);
		if (cpuid_val != 0)
			errx(1, "How did cpuid get re-enabled on fork?");

		printf("exec\n");
		execl("/proc/self/exe", "cpuid-fault", "-early-return", NULL);
	}

	if (child != waitpid(child, &status, 0))
		errx(1, "waitpid failed!?");

	if (WEXITSTATUS(status) != 0)
		errx(1, "Execed child exited abnormally");

	return 0;
}

int child_received_signal;

void child_sigsegv_cb(int sig)
{
	int cpuid_val = 0;

	child_received_signal = 1;
	printf("[ SIG_SEGV ]\n");
	printf("arch_prctl(ARCH_GET_CPUID); ");

	cpuid_val = arch_prctl(ARCH_GET_CPUID, 0);
	if (cpuid_val < 0)
		errx(1, "ARCH_GET_CPUID fails now, but not before?");

	printf("cpuid_val == %s\n", cpuid_names[cpuid_val]);
	printf("arch_prctl(ARCH_SET_CPUID, 1)\n");
	if (arch_prctl(ARCH_SET_CPUID, 1) != 0)
		exit(errno);

	printf("cpuid() == ");
}

int do_child_test(void)
{
	unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;

	signal(SIGSEGV, child_sigsegv_cb);

	/* the child starts out with cpuid disabled, the signal handler
	 * attempts to enable and retry
	 */
	printf("cpuid() == ");
	cpuid(&eax, &ebx, &ecx, &edx);
	printf("{%x, %x, %x, %x}\n", eax, ebx, ecx, edx);
	return child_received_signal ? 0 : 42;
}

int signal_count;

void sigsegv_cb(int sig)
{
	int cpuid_val = 0;

	signal_count++;
	printf("[ SIG_SEGV ]\n");
	printf("arch_prctl(ARCH_GET_CPUID); ");

	cpuid_val = arch_prctl(ARCH_GET_CPUID, 0);
	if (cpuid_val < 0)
		errx(1, "ARCH_GET_CPUID fails now, but not before?");

	printf("cpuid_val == %s\n", cpuid_names[cpuid_val]);
	printf("arch_prctl(ARC_SET_CPUID, 1)\n");
	if (arch_prctl(ARCH_SET_CPUID, 1) != 0)
		errx(1, "ARCH_SET_CPUID failed!");

	printf("cpuid() == ");
}

int main(int argc, char **argv)
{
	int cpuid_val = 0, child = 0, status = 0;
	unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;

	signal(SIGSEGV, sigsegv_cb);
	setvbuf(stdout, NULL, _IONBF, 0);

	cpuid(&eax, &ebx, &ecx, &edx);
	printf("cpuid() == {%x, %x, %x, %x}\n", eax, ebx, ecx, edx);
	printf("arch_prctl(ARCH_GET_CPUID); ");

	cpuid_val = arch_prctl(ARCH_GET_CPUID, 0);
	if (cpuid_val < 0) {
		if (errno == EINVAL) {
			printf("ARCH_GET_CPUID is unsupported on this kernel.\n");
			fflush(stdout);
			exit(0); /* no ARCH_GET_CPUID on this system */
		} else if (errno == ENODEV) {
			printf("ARCH_GET_CPUID is unsupported on this hardware.\n");
			fflush(stdout);
			exit(0); /* no ARCH_GET_CPUID on this system */
		} else {
			errx(errno, "ARCH_GET_CPUID failed unexpectedly!");
		}
	}

	printf("cpuid_val == %s\n", cpuid_names[cpuid_val]);
	cpuid(&eax, &ebx, &ecx, &edx);
	printf("cpuid() == {%x, %x, %x, %x}\n", eax, ebx, ecx, edx);
	printf("arch_prctl(ARCH_SET_CPUID, 1)\n");

	if (arch_prctl(ARCH_SET_CPUID, 1) != 0) {
		if (errno == EINVAL) {
			printf("ARCH_SET_CPUID is unsupported on this kernel.");
			exit(0); /* no ARCH_SET_CPUID on this system */
		} else if (errno == ENODEV) {
			printf("ARCH_SET_CPUID is unsupported on this hardware.");
			exit(0); /* no ARCH_SET_CPUID on this system */
		} else {
			errx(errno, "ARCH_SET_CPUID failed unexpectedly!");
		}
	}


	cpuid(&eax, &ebx, &ecx, &edx);
	printf("cpuid() == {%x, %x, %x, %x}\n", eax, ebx, ecx, edx);
	printf("arch_prctl(ARCH_SET_CPUID, 0)\n");
	fflush(stdout);

	if (arch_prctl(ARCH_SET_CPUID, 0) == -1)
		errx(1, "ARCH_SET_CPUID failed!");

	printf("cpuid() == ");
	eax = ebx = ecx = edx = 0;
	cpuid(&eax, &ebx, &ecx, &edx);
	printf("{%x, %x, %x, %x}\n", eax, ebx, ecx, edx);
	printf("arch_prctl(ARCH_SET_CPUID, 0)\n");

	if (signal_count != 1)
		errx(1, "cpuid didn't fault!");

	if (arch_prctl(ARCH_SET_CPUID, 0) == -1)
		errx(1, "ARCH_SET_CPUID failed!");

	if (argc > 1)
		exit(0); /* Don't run the whole test again if we were execed */

	printf("do_child_test\n");
	child = fork();
	if (child == 0)
		return do_child_test();

	if (child != waitpid(child, &status, 0))
		errx(1, "waitpid failed!?");

	if (WEXITSTATUS(status) != 0)
		errx(1, "Child exited abnormally!");

	/* The child enabling cpuid should not have affected us */
	printf("cpuid() == ");
	eax = ebx = ecx = edx = 0;
	cpuid(&eax, &ebx, &ecx, &edx);
	printf("{%x, %x, %x, %x}\n", eax, ebx, ecx, edx);
	printf("arch_prctl(ARCH_SET_CPUID, 0)\n");

	if (signal_count != 2)
		errx(1, "cpuid didn't fault!");

	if (arch_prctl(ARCH_SET_CPUID, 0) == -1)
		errx(1, "ARCH_SET_CPUID failed!");

	/* Our ARCH_CPUID_SIGSEGV should not propagate through exec */
	printf("do_child_exec_test\n");
	fflush(stdout);

	child = fork();
	if (child == 0)
		return do_child_exec_test(eax, ebx, ecx, edx);

	if (child != waitpid(child, &status, 0))
		errx(1, "waitpid failed!?");

	if (WEXITSTATUS(status) != 0)
		errx(1, "Child exited abnormally!");

	printf("All tests passed!\n");
	exit(EXIT_SUCCESS);
}
