// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
/* Copyright (c) 2018 Netronome Systems, Inc. */

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/vfs.h>

#include <linux/filter.h>
#include <linux/limits.h>

#include <bpf.h>

#include "main.h"

#ifndef PROC_SUPER_MAGIC
# define PROC_SUPER_MAGIC	0x9fa0
#endif

enum probe_component {
	COMPONENT_UNSPEC,
	COMPONENT_KERNEL,
};

/* Miscellaneous utility functions */

static bool check_procfs(void)
{
	struct statfs st_fs;

	if (statfs("/proc", &st_fs) < 0)
		return false;
	if ((unsigned long)st_fs.f_type != PROC_SUPER_MAGIC)
		return false;

	return true;
}

/* Printing utility functions */

static void
print_bool_feature(const char *feat_name, const char *define_name,
		   const char *plain_name, bool res, const char *define_prefix)
{
	if (json_output)
		jsonw_bool_field(json_wtr, feat_name, res);
	else if (define_prefix)
		printf("#define %s%s%s\n", define_prefix,
		       res ? "" : "NO_", define_name);
	else
		printf("%s is %savailable\n", plain_name, res ? "" : "NOT ");
}

static void
print_start_section(const char *json_title, const char *define_comment,
		    const char *plain_title, const char *define_prefix)
{
	if (json_output) {
		jsonw_name(json_wtr, json_title);
		jsonw_start_object(json_wtr);
	} else if (define_prefix) {
		printf("%s\n", define_comment);
	} else {
		printf("%s\n", plain_title);
	}
}

/* Probing functions */

static int read_procfs(const char *path)
{
	char *endptr, *line = NULL;
	size_t len = 0;
	FILE *fd;
	int res;

	fd = fopen(path, "r");
	if (!fd)
		return -1;

	res = getline(&line, &len, fd);
	fclose(fd);
	if (res < 0)
		return -1;

	errno = 0;
	res = strtol(line, &endptr, 10);
	if (errno || *line == '\0' || *endptr != '\n')
		res = -1;
	free(line);

	return res;
}

static void probe_unprivileged_disabled(const char *define_prefix)
{
	int res;

	res = read_procfs("/proc/sys/kernel/unprivileged_bpf_disabled");
	if (json_output) {
		jsonw_int_field(json_wtr, "unprivileged_bpf_disabled", res);
	} else if (define_prefix) {
		printf("#define %sUNPRIVILEGED_BPF_DISABLED ", define_prefix);
		switch (res) {
		case 0:
			printf("%sUNPRIVILEGED_BPF_DISABLED_OFF\n",
			       define_prefix);
			break;
		case 1:
			printf("%sUNPRIVILEGED_BPF_DISABLED_ON\n",
			       define_prefix);
			break;
		case -1:
			printf("%sUNPRIVILEGED_BPF_DISABLED_UNKNOWN\n",
			       define_prefix);
			break;
		default:
			printf("%d\n", res);
		}
		printf("#define  %sUNPRIVILEGED_BPF_DISABLED_OFF 0\n",
		       define_prefix);
		printf("#define  %sUNPRIVILEGED_BPF_DISABLED_ON 1\n",
		       define_prefix);
		printf("#define  %sUNPRIVILEGED_BPF_DISABLED_UNKNOWN -1\n",
		       define_prefix);
	} else {
		switch (res) {
		case 0:
			printf("bpf() syscall for unprivileged users is enabled\n");
			break;
		case 1:
			printf("bpf() syscall restricted to privileged users\n");
			break;
		case -1:
			printf("Unable to retrieve required privileges for bpf() syscall\n");
			break;
		default:
			printf("bpf() syscall restriction has unknown value %d\n", res);
		}
	}
}

static void probe_jit_enable(const char *define_prefix)
{
	int res;

	res = read_procfs("/proc/sys/net/core/bpf_jit_enable");
	if (json_output) {
		jsonw_int_field(json_wtr, "bpf_jit_enable", res);
	} else if (define_prefix) {
		printf("#define %sJIT_COMPILER_ENABLE ", define_prefix);
		switch (res) {
		case 0:
			printf("%sJIT_COMPILER_ENABLE_OFF\n", define_prefix);
			break;
		case 1:
			printf("%sJIT_COMPILER_ENABLE_ON\n", define_prefix);
			break;
		case 2:
			printf("%sJIT_COMPILER_ENABLE_ON_WITH_DEBUG\n",
			       define_prefix);
			break;
		case -1:
			printf("%sJIT_COMPILER_ENABLE_UNKNOWN\n",
			       define_prefix);
			break;
		default:
			printf("%d\n", res);
		}
		printf("#define  %sJIT_COMPILER_ENABLE_OFF 0\n", define_prefix);
		printf("#define  %sJIT_COMPILER_ENABLE_ON 1\n", define_prefix);
		printf("#define  %sJIT_COMPILER_ENABLE_ON_WITH_DEBUG 2\n",
		       define_prefix);
		printf("#define  %sJIT_COMPILER_ENABLE_UNKNOWN -1\n",
		       define_prefix);
	} else {
		switch (res) {
		case 0:
			printf("JIT compiler is disabled\n");
			break;
		case 1:
			printf("JIT compiler is enabled\n");
			break;
		case 2:
			printf("JIT compiler is enabled with debugging traces in kernel logs\n");
			break;
		case -1:
			printf("Unable to retrieve JIT-compiler status\n");
			break;
		default:
			printf("JIT-compiler status has unknown value %d\n",
			       res);
		}
	}
}

static void probe_jit_harden(const char *define_prefix)
{
	int res;

	res = read_procfs("/proc/sys/net/core/bpf_jit_harden");
	if (json_output) {
		jsonw_int_field(json_wtr, "bpf_jit_harden", res);
	} else if (define_prefix) {
		printf("#define %sJIT_COMPILER_HARDEN ", define_prefix);
		switch (res) {
		case 0:
			printf("%sJIT_COMPILER_HARDEN_OFF\n", define_prefix);
			break;
		case 1:
			printf("%sJIT_COMPILER_HARDEN_FOR_UNPRIVILEGED\n",
			       define_prefix);
			break;
		case 2:
			printf("%sJIT_COMPILER_HARDEN_FOR_ALL_USERS\n",
			       define_prefix);
			break;
		case -1:
			printf("%sJIT_COMPILER_HARDEN_UNKNOWN\n",
			       define_prefix);
			break;
		default:
			printf("%d\n", res);
		}
		printf("#define  %sJIT_COMPILER_HARDEN_OFF 0\n", define_prefix);
		printf("#define  %sJIT_COMPILER_HARDEN_FOR_UNPRIVILEGED 1\n",
		       define_prefix);
		printf("#define  %sJIT_COMPILER_HARDEN_FOR_ALL_USERS 2\n",
		       define_prefix);
		printf("#define  %sJIT_COMPILER_HARDEN_UNKNOWN -1\n",
		       define_prefix);
	} else {
		switch (res) {
		case 0:
			printf("JIT compiler hardening is disabled\n");
			break;
		case 1:
			printf("JIT compiler hardening is enabled for unprivileged users\n");
			break;
		case 2:
			printf("JIT compiler hardening is enabled for all users\n");
			break;
		case -1:
			printf("Unable to retrieve JIT hardening status\n");
			break;
		default:
			printf("JIT hardening status has unknown value %d\n",
			       res);
		}
	}
}

static void probe_jit_kallsyms(const char *define_prefix)
{
	int res;

	res = read_procfs("/proc/sys/net/core/bpf_jit_kallsyms");
	if (json_output) {
		jsonw_int_field(json_wtr, "bpf_jit_kallsyms", res);
	} else if (define_prefix) {
		printf("#define %sJIT_COMPILER_KALLSYMS ", define_prefix);
		switch (res) {
		case 0:
			printf("%sJIT_COMPILER_KALLSYMS_OFF\n", define_prefix);
			break;
		case 1:
			printf("%sJIT_COMPILER_KALLSYMS_FOR_ROOT\n",
			       define_prefix);
			break;
		case -1:
			printf("%sJIT_COMPILER_KALLSYMS_UNKNOWN\n",
			       define_prefix);
			break;
		default:
			printf("%d\n", res);
		}
		printf("#define  %sJIT_COMPILER_KALLSYMS_OFF 0\n",
		       define_prefix);
		printf("#define  %sJIT_COMPILER_KALLSYMS_FOR_ROOT 1\n",
		       define_prefix);
		printf("#define  %sJIT_COMPILER_KALLSYMS_UNKNOWN -1\n",
		       define_prefix);
	} else {
		switch (res) {
		case 0:
			printf("JIT compiler kallsyms exports are disabled\n");
			break;
		case 1:
			printf("JIT compiler kallsyms exports are enabled for root\n");
			break;
		case -1:
			printf("Unable to retrieve JIT kallsyms export status\n");
			break;
		default:
			printf("JIT kallsyms exports status has unknown value %d\n", res);
		}
	}
}

static int probe_kernel_version(const char *define_prefix)
{
	int version, subversion, patchlevel, code = 0;
	struct utsname utsn;

	if (!uname(&utsn))
		if (sscanf(utsn.release, "%d.%d.%d",
			   &version, &subversion, &patchlevel) == 3)
			code = (version << 16) + (subversion << 8) + patchlevel;

	if (json_output)
		jsonw_uint_field(json_wtr, "kernel_version_code", code);
	else if (define_prefix)
		printf("#define %sLINUX_VERSION_CODE %d\n",
		       define_prefix, code);
	else if (code)
		printf("Kernel release is %d.%d.%d\n",
		       version, subversion, patchlevel);
	else
		printf("Unable to parse kernel release number\n");

	return code;
}

static bool probe_bpf_syscall(const char *define_prefix)
{
	bool res;

	bpf_load_program(BPF_PROG_TYPE_UNSPEC, NULL, 0, NULL, 0, NULL, 0);
	res = (errno != ENOSYS);

	print_bool_feature("have_bpf_syscall",
			   "BPF_SYSCALL",
			   "bpf() syscall",
			   res, define_prefix);

	return res;
}

static int do_probe(int argc, char **argv)
{
	enum probe_component target = COMPONENT_UNSPEC;
	const char *define_prefix = NULL;

	/* Detection assumes user has sufficient privileges (CAP_SYS_ADMIN).
	 * Let's approximate, and restrict usage to root user only.
	 */
	if (geteuid()) {
		p_err("please run this command as root user");
		return -1;
	}

	set_max_rlimit();

	while (argc) {
		if (is_prefix(*argv, "kernel")) {
			if (target != COMPONENT_UNSPEC) {
				p_err("component to probe already specified");
				return -1;
			}
			target = COMPONENT_KERNEL;
			NEXT_ARG();
		} else if (is_prefix(*argv, "macros") && !define_prefix) {
			define_prefix = "";
			NEXT_ARG();
		} else if (is_prefix(*argv, "prefix")) {
			if (!define_prefix) {
				p_err("'prefix' argument can only be use after 'macros'");
				return -1;
			}
			if (strcmp(define_prefix, "")) {
				p_err("'prefix' already defined");
				return -1;
			}
			NEXT_ARG();

			if (!REQ_ARGS(1))
				return -1;
			define_prefix = GET_ARG();
		} else {
			p_err("expected no more arguments, 'kernel', 'macros' or 'prefix', got: '%s'?",
			      *argv);
			return -1;
		}
	}

	if (json_output)
		jsonw_start_object(json_wtr);

	switch (target) {
	case COMPONENT_KERNEL:
	case COMPONENT_UNSPEC:
		print_start_section("system_config",
				    "/*** System configuration ***/",
				    "Scanning system configuration...",
				    define_prefix);
		if (check_procfs()) {
			probe_unprivileged_disabled(define_prefix);
			probe_jit_enable(define_prefix);
			probe_jit_harden(define_prefix);
			probe_jit_kallsyms(define_prefix);
		} else {
			p_info("/* procfs not mounted, skipping related probes */");
		}
		if (json_output)
			jsonw_end_object(json_wtr);
		else
			printf("\n");
		break;
	}

	print_start_section("syscall_config",
			    "/*** System call and kernel version ***/",
			    "Scanning system call and kernel version...",
			    define_prefix);

	probe_kernel_version(define_prefix);
	probe_bpf_syscall(define_prefix);

	if (json_output) {
		/* End current "section" of probes */
		jsonw_end_object(json_wtr);
		/* End root object */
		jsonw_end_object(json_wtr);
	}

	return 0;
}

static int do_help(int argc, char **argv)
{
	if (json_output) {
		jsonw_null(json_wtr);
		return 0;
	}

	fprintf(stderr,
		"Usage: %s %s probe [kernel] [macros [prefix PREFIX]]\n"
		"       %s %s help\n"
		"",
		bin_name, argv[-2], bin_name, argv[-2]);

	return 0;
}

static const struct cmd cmds[] = {
	{ "help",	do_help },
	{ "probe",	do_probe },
	{ 0 }
};

int do_feature(int argc, char **argv)
{
	return cmd_select(cmds, argc, argv, do_help);
}
