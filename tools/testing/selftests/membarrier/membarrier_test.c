// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <linux/membarrier.h>
#include <sys/utsname.h>
#include <syscall.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "../kselftest.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))

struct memb_tests {
	char testname[80];
	int command;
	int flags;
	int exp_ret;
	int exp_errno;
	int enabled;
	int force;
	int force_exp_errno;
	int above;
	int bellow;
};

struct memb_tests mbt[] = {
	{
	 .testname = "cmd_fail\0",
	 .command = -1,
	 .exp_ret = -1,
	 .exp_errno = EINVAL,
	 .enabled = 1,
	 },
	{
	 .testname = "cmd_flags_fail\0",
	 .command = MEMBARRIER_CMD_QUERY,
	 .flags = 1,
	 .exp_ret = -1,
	 .exp_errno = EINVAL,
	 .enabled = 1,
	 },
	{
	 .testname = "cmd_global_success\0",
	 .command = MEMBARRIER_CMD_GLOBAL,
	 .flags = 0,
	 .exp_ret = 0,
	 },
	/*
	 * PRIVATE EXPEDITED (forced)
	 */
	{
	 .testname = "cmd_private_expedited_fail\0",
	 .command = MEMBARRIER_CMD_PRIVATE_EXPEDITED,
	 .flags = 0,
	 .exp_ret = -1,
	 .exp_errno = EPERM,
	 .force = 1,
	 .force_exp_errno = EINVAL,
	 .bellow = KERNEL_VERSION(4, 10, 0),
	 },
	{
	 .testname = "cmd_private_expedited_fail\0",
	 .command = MEMBARRIER_CMD_PRIVATE_EXPEDITED,
	 .flags = 0,
	 .exp_ret = -1,
	 .exp_errno = EPERM,
	 .force = 1,
	 .force_exp_errno = EPERM,
	 .above = KERNEL_VERSION(4, 10, 0),
	 },
	{
	 .testname = "cmd_register_private_expedited_success\0",
	 .command = MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED,
	 .flags = 0,
	 .exp_ret = 0,
	 .force = 1,
	 .force_exp_errno = EINVAL,
	 },
	{
	 .testname = "cmd_private_expedited_success\0",
	 .command = MEMBARRIER_CMD_PRIVATE_EXPEDITED,
	 .flags = 0,
	 .exp_ret = 0,
	 .force = 1,
	 .force_exp_errno = EINVAL,
	 },
	 /*
	  * PRIVATE EXPEDITED SYNC CORE
	  */
	{
	 .testname = "cmd_private_expedited_sync_core_fail\0",
	 .command = MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE,
	 .flags = 0,
	 .exp_ret = -1,
	 .exp_errno = EPERM,
	 },
	{
	 .testname = "cmd_register_private_expedited_sync_core_success\0",
	 .command = MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE,
	 .flags = 0,
	 .exp_ret = 0,
	 },
	{
	 .testname = "cmd_private_expedited_sync_core_success\0",
	 .command = MEMBARRIER_CMD_PRIVATE_EXPEDITED,
	 .flags = 0,
	 .exp_ret = 0,
	 },
	/*
	 * GLOBAL EXPEDITED
	 * global membarrier from a non-registered process is valid
	 */
	{
	 .testname = "cmd_global_expedited_success\0",
	 .command = MEMBARRIER_CMD_GLOBAL_EXPEDITED,
	 .flags = 0,
	 .exp_ret = 0,
	 },
	{
	 .testname = "cmd_register_global_expedited_success\0",
	 .command = MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED,
	 .flags = 0,
	 .exp_ret = 0,
	 },
	{
	 .testname = "cmd_global_expedited_success\0",
	 .command = MEMBARRIER_CMD_GLOBAL_EXPEDITED,
	 .flags = 0,
	 .exp_ret = 0,
	 },
};

static void
info_passed_ok(struct memb_tests test)
{
	ksft_test_result_pass("sys_membarrier(): %s succeeded.\n",
			test.testname);
}

static void
info_passed_unexpectedly(struct memb_tests test)
{
	ksft_test_result_fail("sys_membarrier(): %s passed unexpectedly. "
			"ret = %d with errno %d were expected. (force: %d)\n",
			test.testname, test.exp_ret, test.exp_errno,
			test.force);
}

static void
info_failed_ok(struct memb_tests test)
{
	ksft_test_result_pass("sys_membarrier(): %s failed as expected.\n",
			test.testname);
}

static void
info_failed_not_ok(struct memb_tests test, int gotret, int goterr)
{
	ksft_test_result_fail("sys_membarrier(): %s failed as expected. "
			"ret = %d when expected was %d. "
			"errno = %d when expected was %d. (force: %d)\n",
			test.testname, gotret, test.exp_ret, goterr,
			test.exp_errno, test.force);
}

static void
info_failed_unexpectedly(struct memb_tests test, int gotret, int goterr)
{
	ksft_test_result_fail("sys_membarrier(): %s failed unexpectedly. "
			"Got ret = %d with errno %d. (force: %d)\n",
			test.testname, gotret, goterr, test.force);
}

static void
info_skipped(struct memb_tests test)
{
	ksft_test_result_skip("sys_membarrier(): %s unsupported, "
			"test skipped.\n", test.testname);
}

static int test_get_kversion(void)
{
	char *temp;
	int major, minor, rev;
	struct utsname uts;

	memset(&uts, 0, sizeof(struct utsname));
	if (uname(&uts) < 0)
		ksft_exit_fail_msg("sys_membarrier(): uname() failed\n");

	temp = strtok((char *) &uts.release, ".");
	if (temp == NULL)
		ksft_exit_fail_msg("sys_membarrier(): kver parsing failed\n");

	major = atoi(temp);

	temp = strtok(NULL, ".");
	if (temp == NULL)
		ksft_exit_fail_msg("sys_membarrier(): kver parsing failed\n");

	minor = atoi(temp);

	temp = strtok(NULL, ".");
	if (temp == NULL)
		ksft_exit_fail_msg("sys_membarrier(): kver parsing failed\n");

	rev = atoi(temp);

	return KERNEL_VERSION(major, minor, rev);
}

static int sys_membarrier(int cmd, int flags)
{
	return syscall(__NR_membarrier, cmd, flags);
}

static void test_membarrier_tests(void)
{
	int i, ret;
	int kver = test_get_kversion();

	for (i = 0; i < ARRAY_SIZE(mbt); i++) {

		if (mbt[i].bellow && kver > mbt[i].bellow)
			continue;

		if (mbt[i].above && kver < mbt[i].above)
			continue;

		if (mbt[i].enabled != 1 && mbt[i].force != 1) {
			info_skipped(mbt[i]);
			continue;
		}

		/* membarrier command should be evaluated */
		ret = sys_membarrier(mbt[i].command, mbt[i].flags);

		if (ret == mbt[i].exp_ret) {

			if (ret >= 0)
				info_passed_ok(mbt[i]);
			else {
				if (mbt[i].enabled == 1 && mbt[i].force != 1) {
					if (errno != mbt[i].exp_errno) {
						info_failed_not_ok(mbt[i], ret,
								errno);
					} else
						info_failed_ok(mbt[i]);
				} else {
					if (errno != mbt[i].force_exp_errno) {
						info_failed_not_ok(mbt[i], ret,
								errno);
					} else
						info_failed_ok(mbt[i]);
				}
			}

		} else {
			if (mbt[i].enabled == 1 && mbt[i].force != 1) {
				if (ret >= 0)
					info_passed_unexpectedly(mbt[i]);
				else {
					info_failed_unexpectedly(mbt[i], ret,
							errno);
				}
			} else {
				if (errno == mbt[i].force_exp_errno)
					info_failed_ok(mbt[i]);
				else
					info_failed_not_ok(mbt[i], ret, errno);
			}
		}
	}
}

static int test_membarrier_prepare(void)
{
	int i, ret;

	ret = sys_membarrier(MEMBARRIER_CMD_QUERY, 0);
	if (ret < 0) {
		if (errno == ENOSYS) {
			/*
			 * It is valid to build a kernel with
			 * CONFIG_MEMBARRIER=n. However, this skips the tests.
			 */
			ksft_exit_skip("sys_membarrier(): CONFIG_MEMBARRIER "
					"is disabled.\n");
		}

		ksft_exit_fail_msg("sys_membarrier(): cmd_query failed.\n");
	}

	for (i = 0; i < ARRAY_SIZE(mbt); i++) {
		if ((mbt[i].command > 0) && (ret & mbt[i].command))
			mbt[i].enabled = 1;
	}

	ksft_test_result_pass("sys_membarrier(): cmd_query succeeded.\n");
}

int main(int argc, char **argv)
{
	ksft_print_header();

	test_membarrier_prepare();
	test_membarrier_tests();

	return ksft_exit_pass();
}
