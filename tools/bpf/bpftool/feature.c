// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
/* Copyright (c) 2018 Netronome Systems, Inc. */

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/utsname.h>
#include <sys/vfs.h>

#include <linux/filter.h>
#include <linux/limits.h>

#include <bpf.h>
#include <libbpf.h>

#include "main.h"

#ifndef PROC_SUPER_MAGIC
# define PROC_SUPER_MAGIC	0x9fa0
#endif

enum probe_component {
	COMPONENT_UNSPEC,
	COMPONENT_KERNEL,
	COMPONENT_DEVICE,
};

#define BPF_HELPER_MAKE_ENTRY(name)	[BPF_FUNC_ ## name] = "bpf_" # name
static const char * const helper_name[] = {
	__BPF_FUNC_MAPPER(BPF_HELPER_MAKE_ENTRY)
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
print_bool_feature(const char *feat_name, const char *plain_name,
		   const char *define_name, bool res, const char *define_prefix)
{
	if (json_output)
		jsonw_bool_field(json_wtr, feat_name, res);
	else if (define_prefix)
		printf("#define %s%sHAVE_%s\n", define_prefix,
		       res ? "" : "NO_", define_name);
	else
		printf("%s is %savailable\n", plain_name, res ? "" : "NOT ");
}

static void print_kernel_option(const char *name, const char *value)
{
	char *endptr;
	int res;

	/* No support for C-style ouptut */

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
	} else {
		if (value)
			printf("%s is set to %s\n", name, value);
		else
			printf("%s is not set\n", name);
	}
}

static void
print_start_section(const char *json_title, const char *plain_title,
		    const char *define_comment, const char *define_prefix)
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
print_end_then_start_section(const char *json_title, const char *plain_title,
			     const char *define_comment,
			     const char *define_prefix)
{
	if (json_output)
		jsonw_end_object(json_wtr);
	else
		printf("\n");

	print_start_section(json_title, plain_title, define_comment,
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

static void probe_unprivileged_disabled(void)
{
	int res;

	/* No support for C-style ouptut */

	res = read_procfs("/proc/sys/kernel/unprivileged_bpf_disabled");
	if (json_output) {
		jsonw_int_field(json_wtr, "unprivileged_bpf_disabled", res);
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

static void probe_jit_enable(void)
{
	int res;

	/* No support for C-style ouptut */

	res = read_procfs("/proc/sys/net/core/bpf_jit_enable");
	if (json_output) {
		jsonw_int_field(json_wtr, "bpf_jit_enable", res);
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

static void probe_jit_harden(void)
{
	int res;

	/* No support for C-style ouptut */

	res = read_procfs("/proc/sys/net/core/bpf_jit_harden");
	if (json_output) {
		jsonw_int_field(json_wtr, "bpf_jit_harden", res);
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

static void probe_jit_kallsyms(void)
{
	int res;

	/* No support for C-style ouptut */

	res = read_procfs("/proc/sys/net/core/bpf_jit_kallsyms");
	if (json_output) {
		jsonw_int_field(json_wtr, "bpf_jit_kallsyms", res);
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

static void probe_kernel_image_config(void)
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
		print_kernel_option(options[i], value);
		free(value);
	}
	fclose(fd);
	return;

no_config:
	for (i = 0; i < ARRAY_SIZE(options); i++)
		print_kernel_option(options[i], NULL);
}

static int probe_kernel_version(const char *define_prefix)
{
	int version, subversion, patchlevel, code = 0;
	struct utsname utsn;

	if (!uname(&utsn))
		if (sscanf(utsn.release, "%d.%d.%d",
			   &version, &subversion, &patchlevel) == 3)
			code = (version << 16) + (subversion << 8) + patchlevel;

	if (define_prefix)
		/* Nothing currently displayed for this ouptut */
		return code;

	if (json_output)
		jsonw_uint_field(json_wtr, "kernel_version_code", code);
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
			   "bpf() syscall",
			   "BPF_SYSCALL",
			   res, define_prefix);

	return res;
}

static void
probe_prog_type(enum bpf_prog_type prog_type, int kernel_version,
		bool *supported_types, const char *define_prefix, __u32 ifindex)
{
	char feat_name[128], plain_desc[128], define_name[128];
	const char *plain_comment = "eBPF program_type ";
	size_t maxlen;
	bool res;

	if (ifindex)
		/* Only test offload-able program types */
		switch (prog_type) {
		case BPF_PROG_TYPE_SCHED_CLS:
		case BPF_PROG_TYPE_XDP:
			break;
		default:
			return;
		}

	res = bpf_probe_prog_type(prog_type, kernel_version, ifindex);

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
	print_bool_feature(feat_name, plain_desc, define_name, res,
			   define_prefix);
}

static void
probe_map_type(enum bpf_map_type map_type, const char *define_prefix,
	       __u32 ifindex)
{
	char feat_name[128], plain_desc[128], define_name[128];
	const char *plain_comment = "eBPF map_type ";
	size_t maxlen;
	bool res;

	res = bpf_probe_map_type(map_type, ifindex);

	maxlen = sizeof(plain_desc) - strlen(plain_comment) - 1;
	if (strlen(map_type_name[map_type]) > maxlen) {
		p_info("map type name too long");
		return;
	}

	sprintf(feat_name, "have_%s_map_type", map_type_name[map_type]);
	sprintf(define_name, "%s_map_type", map_type_name[map_type]);
	uppercase(define_name, sizeof(define_name));
	sprintf(plain_desc, "%s%s", plain_comment, map_type_name[map_type]);
	print_bool_feature(feat_name, plain_desc, define_name, res,
			   define_prefix);
}

static void
probe_helper(__u32 id, const char *name, int kernel_version,
	     bool *supported_types, const char *define_prefix, __u32 ifindex)
{
	char feat_name[128], plain_desc[128], define_name[128];
	unsigned int i;

	sprintf(feat_name, "%s_compat_list", name);
	sprintf(define_name, "%s_helper_compat_list", name);
	uppercase(define_name, sizeof(define_name));
	sprintf(plain_desc,
		"eBPF helper %s supported for program types:",
		name);

	if (json_output) {
		jsonw_name(json_wtr, feat_name);
		jsonw_start_array(json_wtr);
	} else if (define_prefix) {
		printf("#define %s%s\t\"\"", define_prefix, define_name);
	} else {
		printf("%s", plain_desc);
	}

	for (i = BPF_PROG_TYPE_UNSPEC + 1;
	     i < ARRAY_SIZE(prog_type_name); i++) {
		if (!supported_types[i])
			continue;

		if (!bpf_probe_helper(id, i, kernel_version, 0))
			continue;

		if (json_output)
			jsonw_string(json_wtr, prog_type_name[i]);
		else if (define_prefix)
			printf("\t\\\n\t\"%s \"", prog_type_name[i]);
		else
			printf(" %s", prog_type_name[i]);
	}

	if (json_output)
		jsonw_end_array(json_wtr);
	else /* For both C-style and plain output */
		printf("\n");
}

static int do_probe(int argc, char **argv)
{
	enum probe_component target = COMPONENT_UNSPEC;
	const char *define_prefix = NULL;
	bool supported_types[128] = {};
	int kernel_version;
	__u32 ifindex = 0;
	unsigned int i;
	char *ifname;

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
		} else if (is_prefix(*argv, "dev")) {
			NEXT_ARG();

			if (target != COMPONENT_UNSPEC || ifindex) {
				p_err("component to probe already specified");
				return -1;
			}
			if (!REQ_ARGS(1))
				return -1;

			target = COMPONENT_DEVICE;
			ifname = GET_ARG();
			ifindex = if_nametoindex(ifname);
			if (!ifindex) {
				p_err("unrecognized netdevice '%s': %s", ifname,
				      strerror(errno));
				return -1;
			}
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
			p_err("expected no more arguments, 'kernel', 'dev', 'macros' or 'prefix', got: '%s'?",
			      *argv);
			return -1;
		}
	}

	if (json_output) {
		define_prefix = NULL;
		jsonw_start_object(json_wtr);
	}

	switch (target) {
	case COMPONENT_KERNEL:
	case COMPONENT_UNSPEC:
		if (define_prefix)
			break;

		print_start_section("system_config",
				    "Scanning system configuration...",
				    NULL, /* define_comment never used here */
				    NULL); /* define_prefix always NULL here */
		if (check_procfs()) {
			probe_unprivileged_disabled();
			probe_jit_enable();
			probe_jit_harden();
			probe_jit_kallsyms();
		} else {
			p_info("/* procfs not mounted, skipping related probes */");
		}
		probe_kernel_image_config();
		if (json_output)
			jsonw_end_object(json_wtr);
		else
			printf("\n");
		break;
	default:
		break;
	}

	print_start_section("syscall_config",
			    "Scanning system call and kernel version...",
			    "/*** System call availability ***/",
			    define_prefix);

	/* Get kernel version in all cases, we need it for kprobe programs */
	kernel_version = probe_kernel_version(define_prefix);
	if (!probe_bpf_syscall(define_prefix))
		/* bpf() syscall unavailable, don't probe other BPF features */
		goto exit_close_json;

	print_end_then_start_section("program_types",
				     "Scanning eBPF program types...",
				     "/*** eBPF program types ***/",
				     define_prefix);

	for (i = BPF_PROG_TYPE_UNSPEC + 1; i < ARRAY_SIZE(prog_type_name); i++)
		probe_prog_type(i, kernel_version, supported_types,
				define_prefix, ifindex);

	print_end_then_start_section("map_types",
				     "Scanning eBPF map types...",
				     "/*** eBPF map types ***/",
				     define_prefix);

	for (i = BPF_MAP_TYPE_UNSPEC + 1; i < map_type_name_size; i++)
		probe_map_type(i, define_prefix, ifindex);

	print_end_then_start_section("helpers",
				     "Scanning eBPF helper functions...",
				     "/*** eBPF helper functions ***/",
				     define_prefix);

	for (i = 1; i < ARRAY_SIZE(helper_name); i++)
		probe_helper(i, helper_name[i], kernel_version,
			     supported_types, define_prefix, ifindex);

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
		"Usage: %s %s probe [COMPONENT] [macros [prefix PREFIX]]\n"
		"       %s %s help\n"
		"\n"
		"       COMPONENT := { kernel | dev NAME }\n"
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
