/*
 * Inspired by breakpoint overflow test done by
 * Vince Weaver <vincent.weaver@maine.edu> for perf_event_tests
 * (git://github.com/deater/perf_event_tests)
 */

/*
 * Powerpc needs __SANE_USERSPACE_TYPES__ before <linux/types.h> to select
 * 'int-ll64.h' and avoid compile warnings when printing __u64 with %llu.
 */
#define __SANE_USERSPACE_TYPES__

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <linux/compiler.h>
#include <linux/hw_breakpoint.h>

#include "tests.h"
#include "debug.h"
#include "perf.h"
#include "cloexec.h"

static int fd1;
static int fd2;
static int fd3;
static int overflows;
static int overflows_2;

volatile long the_var;


#if defined (__x86_64__)
extern void __test_function(volatile long *ptr);
asm (
	".globl __test_function\n"
	"__test_function:\n"
	"incq (%rdi)\n"
	"ret\n");
#elif defined (__aarch64__)
extern void __test_function(volatile long *ptr);
asm (
	".globl __test_function\n"
	"__test_function:\n"
	"str x30, [x0]\n"
	"ret\n");

#else
static void __test_function(volatile long *ptr)
{
	*ptr++;
}
#endif

__attribute__ ((noinline))
static int test_function(void)
{
	__test_function(&the_var);
	the_var++;
	return time(NULL);
}

static void sig_handler_2(int signum __maybe_unused,
			  siginfo_t *oh __maybe_unused,
			  void *uc __maybe_unused)
{
	overflows_2++;
	if (overflows_2 > 10) {
		ioctl(fd1, PERF_EVENT_IOC_DISABLE, 0);
		ioctl(fd2, PERF_EVENT_IOC_DISABLE, 0);
		ioctl(fd3, PERF_EVENT_IOC_DISABLE, 0);
	}
}

static void sig_handler(int signum __maybe_unused,
			siginfo_t *oh __maybe_unused,
			void *uc __maybe_unused)
{
	overflows++;

	if (overflows > 10) {
		/*
		 * This should be executed only once during
		 * this test, if we are here for the 10th
		 * time, consider this the recursive issue.
		 *
		 * We can get out of here by disable events,
		 * so no new SIGIO is delivered.
		 */
		ioctl(fd1, PERF_EVENT_IOC_DISABLE, 0);
		ioctl(fd2, PERF_EVENT_IOC_DISABLE, 0);
		ioctl(fd3, PERF_EVENT_IOC_DISABLE, 0);
	}
}

static int __xp_event(bool is_bp, void *addr, int setup_signal, int signal)
{
	struct perf_event_attr pe;
	int fd;

	memset(&pe, 0, sizeof(struct perf_event_attr));
	pe.type = PERF_TYPE_BREAKPOINT;
	pe.size = sizeof(struct perf_event_attr);

	pe.config = 0;
	pe.bp_type = is_bp ? HW_BREAKPOINT_X : HW_BREAKPOINT_W;
	pe.bp_addr = (unsigned long) addr;
	pe.bp_len = sizeof(long);

	pe.sample_period = 1;
	pe.sample_type = PERF_SAMPLE_IP;
	pe.wakeup_events = 1;

	pe.disabled = 1;
	pe.exclude_kernel = 1;
	pe.exclude_hv = 1;

	fd = sys_perf_event_open(&pe, 0, -1, -1,
				 perf_event_open_cloexec_flag());
	if (fd < 0) {
		pr_debug("failed opening event %llx\n", pe.config);
		return TEST_FAIL;
	}

	if (setup_signal) {
		fcntl(fd, F_SETFL, O_RDWR|O_NONBLOCK|O_ASYNC);
		fcntl(fd, F_SETSIG, signal);
		fcntl(fd, F_SETOWN, getpid());
	}

	ioctl(fd, PERF_EVENT_IOC_RESET, 0);

	return fd;
}

static int bp_event(void *addr, int setup_signal)
{
	return __xp_event(true, addr, setup_signal, SIGIO);
}

static int wp_event(void *addr, int setup_signal)
{
	return __xp_event(false, addr, setup_signal, SIGIO);
}

static long long bp_count(int fd)
{
	long long count;
	int ret;

	ret = read(fd, &count, sizeof(long long));
	if (ret != sizeof(long long)) {
		pr_debug("failed to read: %d\n", ret);
		return TEST_FAIL;
	}

	return count;
}

int test__bp_signal(int subtest __maybe_unused)
{
	struct sigaction sa;
	long long count1, count2, count3;

	/* setup SIGIO signal handler */
	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_sigaction = (void *) sig_handler;
	sa.sa_flags = SA_SIGINFO;

	if (sigaction(SIGIO, &sa, NULL) < 0) {
		pr_debug("failed setting up signal handler\n");
		return TEST_FAIL;
	}

	sa.sa_sigaction = (void *) sig_handler_2;
	if (sigaction(SIGUSR1, &sa, NULL) < 0) {
		pr_debug("failed setting up signal handler 2\n");
		return TEST_FAIL;
	}

	/*
	 * We create following events:
	 *
	 * fd1 - breakpoint event on test_function with SIGIO
	 *       signal configured. We should get signal
	 *       notification each time the breakpoint is hit
	 *
	 * fd2 - breakpoint event on sig_handler with SIGUSR1
	 *       configured. We should get SIGUSR1 each time when
	 *       breakpoint is hit
	 *
	 * fd3 - watchpoint event on test_function with SIGIO
	 *       configured.
	 *
	 * Following processing should happen:
	 *   - execute test_function
	 *   - fd1 event breakpoint hit -> count1 == 1
	 *   - SIGIO is delivered       -> overflows == 1
	 *   - fd2 event breakpoint hit -> count2 == 1
	 *   - SIGUSR1 is delivered     -> overflows_2 == 1  (nested signal)
	 *   - sig_handler_2 return
	 *   - sig_handler return
	 *   - fd3 event watchpoint hit -> count3 == 1       (wp and bp in one insn)
	 *   - SIGIO is delivered       -> overflows == 2
	 *   - fd2 event breakpoint hit -> count2 == 2
	 *   - SIGUSR1 is delivered     -> overflows_2 == 2
	 *   - sig_handler_2 return
	 *   - sig_handler return
	 *   - fd3 event watchpoint hit -> count3 == 2       (standalone wp)
	 *   - SIGIO is delivered       -> overflows = 3
	 *   - fd2 event breakpoint hit -> count2 == 3
	 *   - SIGUSR1 is delivered     -> overflows_2 == 3
	 *   - sig_handler_2 return
	 *   - sig_handler return
	 *
	 * The test case check following error conditions:
	 * - we get stuck in signal handler because of debug
	 *   exception being triggered receursively due to
	 *   the wrong RF EFLAG management
	 *
	 * - we never trigger the sig_handler breakpoint due
	 *   to the rong RF EFLAG management
	 *
	 */

	fd1 = bp_event(__test_function, 1);
	fd2 = __xp_event(true, sig_handler, 1, SIGUSR1);
	fd3 = wp_event((void *)&the_var, 1);

	ioctl(fd1, PERF_EVENT_IOC_ENABLE, 0);
	ioctl(fd2, PERF_EVENT_IOC_ENABLE, 0);
	ioctl(fd3, PERF_EVENT_IOC_ENABLE, 0);

	/*
	 * Kick off the test by trigering 'fd1'
	 * breakpoint.
	 */
	test_function();

	ioctl(fd1, PERF_EVENT_IOC_DISABLE, 0);
	ioctl(fd2, PERF_EVENT_IOC_DISABLE, 0);
	ioctl(fd3, PERF_EVENT_IOC_DISABLE, 0);

	count1 = bp_count(fd1);
	count2 = bp_count(fd2);
	count3 = bp_count(fd3);

	close(fd1);
	close(fd2);
	close(fd3);

	pr_debug("count1 %lld, count2 %lld, count3 %lld, overflow %d, overflows_2 %d\n",
		 count1, count2, count3, overflows, overflows_2);

	if (count1 != 1) {
		if (count1 == 11)
			pr_debug("failed: RF EFLAG recursion issue detected\n");
		else
			pr_debug("failed: wrong count for bp1%lld\n", count1);
	}

	if (overflows != 3)
		pr_debug("failed: wrong overflow hit\n");

	if (overflows_2 != 3)
		pr_debug("failed: wrong overflow_2 hit\n");

	if (count2 != 3)
		pr_debug("failed: wrong count for bp2\n");

	if (count3 != 2)
		pr_debug("failed: wrong count for bp3\n");

	return count1 == 1 && overflows == 3 && count2 == 3 && overflows_2 == 3 && count3 == 2 ?
		TEST_OK : TEST_FAIL;
}
