
/*
 * Tests for arch_prctl(ARCH_GET_CPUID, ...) / prctl(ARCH_SET_CPUID, ...)
 *
 * Basic test to test behaviour of ARCH_GET_CPUID and ARCH_SET_CPUID
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <inttypes.h>
#include <cpuid.h>
#include <errno.h>
#include <sys/wait.h>

#include <sys/prctl.h>
#include <linux/prctl.h>

const char *cpuid_names[] = {
	[0] = "[not set]",
	[ARCH_CPUID_ENABLE] = "ARCH_CPUID_ENABLE",
	[ARCH_CPUID_SIGSEGV] = "ARCH_CPUID_SIGSEGV",
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

	printf("arch_prctl(ARCH_GET_CPUID, &cpuid_val); ");
	fflush(stdout);

	if (arch_prctl(ARCH_GET_CPUID, (unsigned long)&cpuid_val) != 0)
		exit(42);

	printf("cpuid_val == %s\n", cpuid_names[cpuid_val]);
	if (cpuid_val != ARCH_CPUID_SIGSEGV)
		exit(42);

	if ((child = fork()) == 0) {
		printf("exec\n");
		fflush(stdout);
		execl("/proc/self/exe", "cpuid-fault", "-early-return", NULL);
	}

	if (child != waitpid(child, &status, 0))
		exit(42);

	if (WEXITSTATUS(status) != 0)
		exit(42);

	return 0;
}

int child_received_signal;

void child_sigsegv_cb(int sig)
{
	int cpuid_val = 0;

	child_received_signal = 1;
	printf("[ SIG_SEGV ]\n");
	printf("arch_prctl(ARCH_GET_CPUID, &cpuid_val); ");
	fflush(stdout);

	if (arch_prctl(ARCH_GET_CPUID, (unsigned long)&cpuid_val) != 0)
		exit(42);

	printf("cpuid_val == %s\n", cpuid_names[cpuid_val]);
	printf("arch_prctl(ARCH_SET_CPUID, ARCH_CPUID_ENABLE)\n");
	fflush(stdout);
	if (arch_prctl(ARCH_SET_CPUID, ARCH_CPUID_ENABLE) != 0)
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
	fflush(stdout);
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
	printf("arch_prctl(ARCH_GET_CPUID, &cpuid_val); ");
	fflush(stdout);

	if (arch_prctl(ARCH_GET_CPUID, (unsigned long)&cpuid_val) != 0)
		exit(42);

	printf("cpuid_val == %s\n", cpuid_names[cpuid_val]);
	printf("arch_prctl(ARC_SET_CPUID, ARCH_CPUID_ENABLE)\n");
	fflush(stdout);
	if (arch_prctl(ARCH_SET_CPUID, ARCH_CPUID_ENABLE) != 0)
		exit(42);

	printf("cpuid() == ");
}

int main(int argc, char** argv)
{
	int cpuid_val = 0, child = 0, status = 0;
	unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;

	signal(SIGSEGV, sigsegv_cb);

	cpuid(&eax, &ebx, &ecx, &edx);
	printf("cpuid() == {%x, %x, %x, %x}\n", eax, ebx, ecx, edx);
	printf("arch_prctl(ARCH_GET_CPUID, &cpuid_val); ");
	fflush(stdout);

	if (arch_prctl(ARCH_GET_CPUID, (unsigned long)&cpuid_val) != 0) {
		if (errno == EINVAL) {
			printf("ARCH_GET_CPUID is unsupported on this system.");
			fflush(stdout);
			exit(0); /* no ARCH_GET_CPUID on this system */
		} else {
			exit(42);
		}
	}

	printf("cpuid_val == %s\n", cpuid_names[cpuid_val]);
	cpuid(&eax, &ebx, &ecx, &edx);
	printf("cpuid() == {%x, %x, %x, %x}\n", eax, ebx, ecx, edx);
	printf("arch_prctl(ARCH_SET_CPUID, ARCH_CPUID_ENABLE)\n");
	fflush(stdout);

	if (arch_prctl(ARCH_SET_CPUID, ARCH_CPUID_ENABLE) != 0) {
		if (errno == EINVAL) {
			printf("ARCH_SET_CPUID is unsupported on this system.");
			fflush(stdout);
			exit(0); /* no ARCH_SET_CPUID on this system */
		} else {
			exit(42);
		}
	}


	cpuid(&eax, &ebx, &ecx, &edx);
	printf("cpuid() == {%x, %x, %x, %x}\n", eax, ebx, ecx, edx);
	printf("arch_prctl(ARCH_SET_CPUID, ARCH_CPUID_SIGSEGV)\n");
	fflush(stdout);

	if (arch_prctl(ARCH_SET_CPUID, ARCH_CPUID_SIGSEGV) == -1)
		exit(42);

	printf("cpuid() == ");
	fflush(stdout);
	eax = ebx = ecx = edx = 0;
	cpuid(&eax, &ebx, &ecx, &edx);
	printf("{%x, %x, %x, %x}\n", eax, ebx, ecx, edx);
	printf("arch_prctl(ARCH_SET_CPUID, ARCH_CPUID_SIGSEGV)\n");
	fflush(stdout);

	if (signal_count != 1)
		exit(42);

	if (arch_prctl(ARCH_SET_CPUID, ARCH_CPUID_SIGSEGV) == -1)
		exit(42);

	if (argc > 1)
		exit(0); /* Don't run the whole test again if we were execed */

	printf("do_child_test\n");
	fflush(stdout);
	if ((child = fork()) == 0)
		return do_child_test();

	if (child != waitpid(child, &status, 0))
		exit(42);

	if (WEXITSTATUS(status) != 0)
		exit(42);

	/* The child enabling cpuid should not have affected us */
	printf("cpuid() == ");
	fflush(stdout);
	eax = ebx = ecx = edx = 0;
	cpuid(&eax, &ebx, &ecx, &edx);
	printf("{%x, %x, %x, %x}\n", eax, ebx, ecx, edx);
	printf("arch_prctl(ARCH_SET_CPUID, ARCH_CPUID_SIGSEGV)\n");
	fflush(stdout);

	if (signal_count != 2)
		exit(42);

	if (arch_prctl(ARCH_SET_CPUID, ARCH_CPUID_SIGSEGV) == -1)
		exit(42);

	/* Our ARCH_CPUID_SIGSEGV should not propagate through exec */
	printf("do_child_exec_test\n");
	fflush(stdout);
	if ((child = fork()) == 0)
		return do_child_exec_test(eax, ebx, ecx, edx);

	if (child != waitpid(child, &status, 0))
		exit(42);

	if (WEXITSTATUS(status) != 0)
		exit(42);

	printf("All tests passed!\n");
	fflush(stdout);
	exit(EXIT_SUCCESS);
}

