// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
/* Copyright (c) 2018 Netronome Systems, Inc. */

#include <ctype.h>
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

static void uppercase(char *str, size_t len)
{
	size_t i;

	for (i = 0; i < len && str[i] != '\0'; i++)
		str[i] = toupper(str[i]);
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
print_kernel_option(const char *name, const char *value,
		    const char *define_prefix)
{
	char *endptr;
	int res;

	if (json_output) {
		if (!value) {
			jsonw_null_field(json_wtr, name);
			return;
		}
		errno = 0;
		res = strtol(value, &endptr, 0);
		if (!errno && *endptr == '\n')
			jsonw_int_field(json_wtr, name, res);
		else
			jsonw_string_field(json_wtr, name, value);
	} else if (define_prefix) {
		if (value)
			printf("#define %s%s %s\n", define_prefix, name, value);
		else
			printf("#define %sNO_%s\n", define_prefix, name);
	} else {
		if (value)
			printf("%s is set to %s\n", name, value);
		else
			printf("%s is not set\n", name);
	}
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

static void
print_end_then_start_section(const char *json_title, const char *define_title,
			     const char *plain_title, const char *define_prefix)
{
	if (json_output)
		jsonw_end_object(json_wtr);
	else
		printf("\n");

	print_start_section(json_title, define_title, plain_title,
			    define_prefix);
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

static char *get_kernel_config_option(FILE *fd, const char *option)
{
	size_t line_n = 0, optlen = strlen(option);
	char *res, *strval, *line = NULL;
	ssize_t n;

	rewind(fd);
	while ((n = getline(&line, &line_n, fd)) > 0) {
		if (strncmp(line, option, optlen))
			continue;
		/* Check we have at least '=', value, and '\n' */
		if (strlen(line) < optlen + 3)
			continue;
		if (*(line + optlen) != '=')
			continue;

		/* Trim ending '\n' */
		line[strlen(line) - 1] = '\0';

		/* Copy and return config option value */
		strval = line + optlen + 1;
		res = strdup(strval);
		free(line);
		return res;
	}
	free(line);

	return NULL;
}

static void probe_kernel_image_config(const char *define_prefix)
{
	const char * const options[] = {
		"CONFIG_BPF",
		"CONFIG_BPF_SYSCALL",
		"CONFIG_HAVE_EBPF_JIT",
		"CONFIG_BPF_JIT",
		"CONFIG_BPF_JIT_ALWAYS_ON",
		"CONFIG_NET",
		"CONFIG_XDP_SOCKETS",
		"CONFIG_CGROUPS",
		"CONFIG_CGROUP_BPF",
		"CONFIG_CGROUP_NET_CLASSID",
		"CONFIG_BPF_EVENTS",
		"CONFIG_LWTUNNEL_BPF",
		"CONFIG_NET_ACT_BPF",
		"CONFIG_NET_CLS_ACT",
		"CONFIG_NET_CLS_BPF",
		"CONFIG_NET_SCH_INGRESS",
		"CONFIG_XFRM",
		"CONFIG_SOCK_CGROUP_DATA",
		"CONFIG_IP_ROUTE_CLASSID",
		"CONFIG_IPV6_SEG6_BPF",
		"CONFIG_FUNCTION_ERROR_INJECTION",
		"CONFIG_BPF_KPROBE_OVERRIDE",
		"CONFIG_BPF_LIRC_MODE2",
		"CONFIG_NETFILTER_XT_MATCH_BPF",
		"CONFIG_TEST_BPF",
		"CONFIG_BPFILTER",
		"CONFIG_BPFILTER_UMH",
		"CONFIG_BPF_STREAM_PARSER",
	};
	char *value, *buf = NULL;
	struct utsname utsn;
	char path[PATH_MAX];
	size_t i, n;
	ssize_t ret;
	FILE *fd;

	if (uname(&utsn))
		goto no_config;

	snprintf(path, sizeof(path), "/boot/config-%s", utsn.release);

	fd = fopen(path, "r");
	if (!fd && errno == ENOENT) {
		/* Sometimes config is at /proc/config */
		fd = fopen("/proc/config", "r");
	}
	if (!fd) {
		p_err("can't open kernel config file: %s", strerror(errno));
		goto no_config;
	}
	/* Sanity checks */
	ret = getline(&buf, &n, fd);
	ret = getline(&buf, &n, fd);
	if (!buf || !ret) {
		p_err("can't read from kernel config file: %s",
		      strerror(errno));
		free(buf);
		goto no_config;
	}
	if (strcmp(buf, "# Automatically generated file; DO NOT EDIT.\n")) {
		p_err("can't find correct kernel config file");
		free(buf);
		goto no_config;
	}
	free(buf);

	for (i = 0; i < ARRAY_SIZE(options); i++) {
		value = get_kernel_config_option(fd, options[i]);
		print_kernel_option(options[i], value, define_prefix);
		free(value);
	}
	fclose(fd);
	return;

no_config:
	for (i = 0; i < ARRAY_SIZE(options); i++)
		print_kernel_option(options[i], NULL, define_prefix);
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

static void
prog_load(enum bpf_prog_type prog_type, const struct bpf_insn *insns,
	  size_t insns_cnt, int kernel_version, char *buf, size_t buf_len)
{
	struct bpf_load_program_attr xattr = {};
	int fd;

	switch (prog_type) {
	case BPF_PROG_TYPE_CGROUP_SOCK_ADDR:
		xattr.expected_attach_type = BPF_CGROUP_INET4_CONNECT;
		break;
	default:
		break;
	}

	xattr.prog_type = prog_type;
	xattr.insns = insns;
	xattr.insns_cnt = insns_cnt;
	xattr.license = "GPL";
	xattr.kern_version = kernel_version;

	fd = bpf_load_program_xattr(&xattr, buf, buf_len);
	if (fd >= 0)
		close(fd);
}

static void
probe_prog_type(enum bpf_prog_type prog_type, int kernel_version,
		bool *supported_types, const char *define_prefix)
{
	char buf[4096], feat_name[128], define_name[128], plain_desc[128];
	const char *plain_comment = "eBPF program_type ";
	struct bpf_insn insns[2] = {
		BPF_MOV64_IMM(BPF_REG_0, 0),
		BPF_EXIT_INSN()
	};
	size_t maxlen;
	bool res;

	errno = 0;
	prog_load(prog_type, insns, ARRAY_SIZE(insns), kernel_version,
		  buf, sizeof(buf));
	res = (errno != EINVAL && errno != EOPNOTSUPP);

	supported_types[prog_type] |= res;

	maxlen = sizeof(plain_desc) - strlen(plain_comment) - 1;
	if (strlen(prog_type_name[prog_type]) > maxlen) {
		p_info("program type name too long");
		return;
	}

	sprintf(feat_name, "have_%s_prog_type", prog_type_name[prog_type]);
	sprintf(define_name, "%s_prog_type", prog_type_name[prog_type]);
	uppercase(define_name, sizeof(define_name));
	sprintf(plain_desc, "%s%s", plain_comment, prog_type_name[prog_type]);
	print_bool_feature(feat_name, define_name, plain_desc, res,
			   define_prefix);
}

static void
probe_map_type(enum bpf_map_type map_type, const char *define_prefix)
{
	char feat_name[128], define_name[128], plain_desc[128];
	int key_size, value_size, max_entries, map_flags;
	const char *plain_comment = "eBPF map_type ";
	struct bpf_create_map_attr attr = {};
	int fd = -1, fd_inner;
	size_t maxlen;
	bool res;

	key_size = sizeof(__u32);
	value_size = sizeof(__u32);
	max_entries = 1;
	map_flags = 0;

	switch (map_type) {
	case BPF_MAP_TYPE_LPM_TRIE:
		key_size = sizeof(__u64);
		value_size = sizeof(__u64);
		map_flags = BPF_F_NO_PREALLOC;
		break;
	case BPF_MAP_TYPE_STACK_TRACE:
		value_size = sizeof(__u64);
		break;
	case BPF_MAP_TYPE_CGROUP_STORAGE:
	case BPF_MAP_TYPE_PERCPU_CGROUP_STORAGE:
		key_size = sizeof(struct bpf_cgroup_storage_key);
		value_size = sizeof(__u64);
		max_entries = 0;
		break;
	case BPF_MAP_TYPE_QUEUE:
	case BPF_MAP_TYPE_STACK:
		key_size = 0;
		break;
	default:
		break;
	}

	switch (map_type) {
	case BPF_MAP_TYPE_ARRAY_OF_MAPS:
	case BPF_MAP_TYPE_HASH_OF_MAPS:
		fd_inner = bpf_create_map(BPF_MAP_TYPE_HASH,
					  sizeof(__u32), sizeof(__u32), 1, 0);
		if (fd_inner < 0)
			break;
		fd = bpf_create_map_in_map(map_type, "", sizeof(__u32),
					   fd_inner, 1, 0);
		close(fd_inner);
		break;
	default:
		/* Note: No other restriction on map type probes for offload */
		attr.map_type = map_type;
		attr.key_size = key_size;
		attr.value_size = value_size;
		attr.max_entries = max_entries;
		attr.map_flags = map_flags;

		fd = bpf_create_map_xattr(&attr);
		break;
	}
	if (fd >= 0)
		close(fd);

	res = fd >= 0;

	maxlen = sizeof(plain_desc) - strlen(plain_comment) - 1;
	if (strlen(map_type_name[map_type]) > maxlen) {
		p_info("map type name too long");
		return;
	}

	sprintf(feat_name, "have_%s_map_type", map_type_name[map_type]);
	sprintf(define_name, "%s_map_type", map_type_name[map_type]);
	uppercase(define_name, sizeof(define_name));
	sprintf(plain_desc, "%s%s", plain_comment, map_type_name[map_type]);
	print_bool_feature(feat_name, define_name, plain_desc, res,
			   define_prefix);
}

static int do_probe(int argc, char **argv)
{
	enum probe_component target = COMPONENT_UNSPEC;
	const char *define_prefix = NULL;
	bool supported_types[128] = {};
	int kernel_version;
	unsigned int i;

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
		probe_kernel_image_config(define_prefix);
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

	kernel_version = probe_kernel_version(define_prefix);
	if (!probe_bpf_syscall(define_prefix))
		/* bpf() syscall unavailable, don't probe other BPF features */
		goto exit_close_json;

	print_end_then_start_section("program_types",
				     "/*** eBPF program types ***/",
				     "Scanning eBPF program types...",
				     define_prefix);

	for (i = BPF_PROG_TYPE_SOCKET_FILTER;
	     i < ARRAY_SIZE(prog_type_name); i++)
		probe_prog_type(i, kernel_version, supported_types,
				define_prefix);

	print_end_then_start_section("map_types",
				     "/*** eBPF map types ***/",
				     "Scanning eBPF map types...",
				     define_prefix);

	for (i = BPF_MAP_TYPE_HASH; i < map_type_name_size; i++)
		probe_map_type(i, define_prefix);

exit_close_json:
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
