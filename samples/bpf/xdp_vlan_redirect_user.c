// SPDX-License-Identifier: GPL-2.0
/*  XDP redirect vlans to CPUs - user code
 *
 *  Copyright(c) 2018 Oracle Corp.
 *
 *
 * This _user.c code, along with the accompanying _kern.c, is
 * intended as an example of using XDP to redirect processing of
 * particular vlan packets to specific CPUs.  This is in response
 * to comments received on a kernel patch put forth previously
 * to do something similar using RPS.
 *     https://www.spinics.net/lists/netdev/msg528210.html
 *     [PATCH net-next] net: enable RPS on vlan devices
 *
 * This XDP application watches for inbound vlan-tagged packets
 * and redirects those packets to be processed on a specific CPU
 * as configured in a BPF map.  The BPF map can be modified by
 * this user program, which can also load and unload the kernel
 * XDP code.
 *
 * In supporting VMs where we can't control the OS being used,
 * we'd like to separate the VM CPU processing from the host's
 * CPUs as a way to help mitigate the impact of the L1TF issue.
 * When running the VM's traffic on a vlan we can stick the Rx
 * processing on one set of CPUs separate from the VM's CPUs.
 * Yes, choosing to use this may cause a bit of throughput pain
 * when the packets are actually passed into the VM and have to
 * move from one cache to another.
 *
 * This example currently uses a vlan key and cpu value in the
 * BPF map, so only can do one CPU per vlan.  This could easily
 * be modified to use a bitpattern of CPUs rather than a CPU id
 * to allow multiple CPUs per vlan.
 *
 * Before using, please be sure to mount the bpf pseudo-fs
 *	mount -t bpf bpf /sys/fs/bpf
 *
 * Also, be sure that the device is not stripping vlan tags
 * so that the XDP program has a chance to inspect them
 *	ethtool -K eth0 rxvlan off
 *
 * To load the feature, use a command line something like this:
 *	xdp_vlan_redirect --dev eth0 --install
 *
 * Once installed, you can see the pinned files in userspace:
 *	# ls /sys/fs/bpf
 *	xdp_vlan_redirect  xdp_vlan_redirect_map
 *
 * These commands add vlan:cpu mappings:
 *	xdp_vlan_redirect --dev eth0 --vlan 1 --cpu 5
 *	xdp_vlan_redirect -d eth0 -v 3 -c 4
 *
 * You can use the bpftool to print the contents of the vlan map:
 *	# bpftool map dump pinned /sys/fs/bpf/xdp_vlan_redirect_vlan_map
 *	key: 00 00 00 00  value: 00 00 00 ff 00 00 00 00
 *	key: 01 00 00 00  value: 05 00 00 00 00 00 00 00
 *	key: 02 00 00 00  value: 00 00 00 ff 00 00 00 00
 *	key: 03 00 00 00  value: 04 00 00 00 00 00 00 00
 *	key: 04 00 00 00  value: 00 00 00 ff 00 00 00 00
 *	    :
 *
 * Use negative numbers to remove vlans from the map:
 *	xdp_vlan_redirect -d eth0 -v -3
 *
 * It is possible to do map editing with bpftool, but note that all
 * the bytes of both the key and the value must be specified:
 *	# bpftool map update pinned /sys/fs/bpf/xdp_vlan_redirect_vlan_map \
 *		  key 3 0 0 0 value 0 7 0 0 0 0 0 0
 *
 * Removing the feature is similar to install:
 *	xdp_vlan_redirect --dev eth0 --remove
 *
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include <sys/resource.h>
#include <getopt.h>
#include <net/if.h>

#include <linux/if_link.h>

#include <bpf/bpf.h>
#include "bpf_load.h"
#include "bpf_util.h"

#define MAX_CPUS  64          /* WARNING - sync with _kern.c */
#define UNDEF_CPU 0xff000000  /* WARNING - sync with _kern.c */

/* counters in the counter_map */
#define VRC_CALLS  0    /* number of calls to this program */
#define VRC_VLANS  1    /* number of vlan packets seen */
#define VRC_HITS   2    /* number of redirects attempted */
#define CPU_COUNT  3    /* number of CPUs found */

static const struct option long_options[] = {
	{"help",	no_argument,		NULL, 'h' },
	{"dev",		required_argument,	NULL, 'd' },
	{"cpu",		required_argument,	NULL, 'c' },
	{"vlan",	required_argument,	NULL, 'v' },
	{"install",	no_argument,		NULL, 'i' },
	{"remove",	no_argument,		NULL, 'r' },
	{0, 0, NULL,  0 }
};

static void usage(char *argv[])
{
	int i;

	printf("%s - CPU targeting for vlan processing\n", argv[0]);
	printf("\n");
	printf(" Usage: %s (options-see-below)\n", argv[0]);
	printf(" Listing options:\n");
	for (i = 0; long_options[i].name != 0; i++) {
		printf(" --%-12s", long_options[i].name);
		if (long_options[i].flag != NULL)
			printf(" flag (internal value:%d)",
				*long_options[i].flag);
		else
			printf(" short-option: -%c",
				long_options[i].val);
		printf("\n");
	}
	printf("\n");
}

int main(int argc, char **argv)
{
	struct rlimit r = {10 * 1024 * 1024, RLIM_INFINITY};
	const char *pinpath = "/sys/fs/bpf/";
	char filename[64] = { 0 };
	char pin_prog_name[sizeof(filename) + sizeof(pinpath)] = { 0 };
	char pin_vlanmap_name[sizeof(filename) + sizeof(pinpath)] = { 0 };
	char pin_countermap_name[sizeof(filename) + sizeof(pinpath)] = { 0 };
	const char *kern_suffix = "_kern.o";
	char ifname[IF_NAMESIZE] = { 0 };
	bool install = false;
	bool remove = false;
	int cpu = MAX_CPUS;
	int longindex = 0;
	int ifindex = -1;
	int vfd, cfd;
	int vlan = 0;
	__u64 v64;
	__u64 c64;
	int ret;
	int opt;

	if (setrlimit(RLIMIT_MEMLOCK, &r)) {
		perror("setrlimit(RLIMIT_MEMLOCK)");
		return 1;
	}

	/* Parse command line args */
	while ((opt = getopt_long(argc, argv, "d:c:v:ri",
				  long_options, &longindex)) != -1) {
		switch (opt) {
		case 'd':
			strncpy(ifname, optarg, sizeof(ifname)-1);
			ifindex = if_nametoindex(ifname);
			if (ifindex == 0) {
				fprintf(stderr, "ERR: device name '%s' : %s\n",
					ifname, strerror(errno));
				goto error;
			}
			break;
		case 'c':
			cpu = strtoul(optarg, NULL, 0);
			if (cpu >= MAX_CPUS) {
				fprintf(stderr, "ERR: invalid cpu id '%d'\n", cpu);
				goto error;
			}
			break;
		case 'v':
			vlan = atoi(optarg);
			if (!vlan || vlan >= 4096) {
				fprintf(stderr, "ERR: invalid vlan id '%d'\n", vlan);
				goto error;
			}
			break;
		case 'i':
			install = true;
			break;
		case 'r':
			remove = true;
			break;
		case 'h':
		default:
			goto error;
		}
	}

	/* Required options */
	if (ifindex == -1) {
		fprintf(stderr, "ERR: required option --dev missing\n");
		goto error;
	}

	if (install && remove) {
		fprintf(stderr, "ERR: pick only one of install or remove\n");
		goto error;
	}

	if ((install || remove) && (vlan || cpu != MAX_CPUS)) {
		fprintf(stderr, "ERR: pick either (install or remove) or vlan and cpu\n");
		goto error;
	}

	if (strlen(argv[0]) > (sizeof(filename) - sizeof(kern_suffix))) {
		fprintf(stderr, "filename %s too long\n", argv[0]);
		return -1;
	}

	snprintf(filename, sizeof(filename), "%s%s", argv[0], kern_suffix);
	snprintf(pin_prog_name, sizeof(pin_prog_name), "%s%s", pinpath, argv[0]);
	snprintf(pin_vlanmap_name, sizeof(pin_vlanmap_name), "%s%s_vlan_map", pinpath, argv[0]);
	snprintf(pin_countermap_name, sizeof(pin_countermap_name), "%s%s_counter_map", pinpath, argv[0]);

	if (install) {

		/* check to see if already installed */
		errno = 0;
		access(pin_prog_name, R_OK);
		if (errno != ENOENT) {
			fprintf(stderr, "ERR: %s is already installed\n", argv[0]);
			return -1;
		}

		/* load the XDP program and maps with the convenient library */
		if (load_bpf_file(filename)) {
			fprintf(stderr, "ERR: load_bpf_file(%s): \n%s",
				filename, bpf_log_buf);
			return -1;
		}
		if (!prog_fd[0]) {
			fprintf(stderr, "ERR: load_bpf_file(%s): %d %s\n",
				filename, errno, strerror(errno));
			return -1;
		}

		/* pin the XDP program and maps */
		if (bpf_obj_pin(prog_fd[0], pin_prog_name) < 0) {
			fprintf(stderr, "ERR: bpf_obj_pin(%s): %d %s\n",
				pin_prog_name, errno, strerror(errno));
			if (errno == 2)
				fprintf(stderr, "     (is the BPF fs mounted on /sys/fs/bpf?)\n");
			return -1;
		}
		if (bpf_obj_pin(map_fd[0], pin_vlanmap_name) < 0) {
			fprintf(stderr, "ERR: bpf_obj_pin(%s): %d %s\n",
				pin_vlanmap_name, errno, strerror(errno));
			return -1;
		}
		if (bpf_obj_pin(map_fd[2], pin_countermap_name) < 0) {
			fprintf(stderr, "ERR: bpf_obj_pin(%s): %d %s\n",
				pin_countermap_name, errno, strerror(errno));
			return -1;
		}

		/* prep the vlan map with "not used" values */
		c64 = UNDEF_CPU;
		for (v64 = 0; v64 < 4096; v64++) {
			if (bpf_map_update_elem(map_fd[0], &v64, &c64, 0)) {
				fprintf(stderr, "ERR: preping vlan map failed on v=%llu: %d %s\n",
					v64, errno, strerror(errno));
				return -1;
			}
		}

		/* prep the cpumap with queue sizes */
		c64 = 128+64;  /* see note in xdp_redirect_cpu_user.c */
		for (v64 = 0; v64 < MAX_CPUS; v64++) {
			if (bpf_map_update_elem(map_fd[1], &v64, &c64, 0)) {
				if (errno == ENODEV) {
					/* Save the last CPU number attempted
					 * into the counters map
					 */
					c64 = CPU_COUNT;
					ret = bpf_map_update_elem(map_fd[2], &c64, &v64, 0);
					break;
				}

				fprintf(stderr, "ERR: preping cpu map failed on v=%llu: %d %s\n",
					v64, errno, strerror(errno));
				return -1;
			}
		}

		/* wire the XDP program to the device */
		if (bpf_set_link_xdp_fd(ifindex, prog_fd[0], 0) < 0) {
			fprintf(stderr, "ERR: bpf_set_link_xdp_fd(): %d %s\n",
				errno, strerror(errno));
			return -1;
		}

		return 0;
	}

	if (remove) {

		/* unlink the program from the device */
		if (bpf_set_link_xdp_fd(ifindex, -1, 0) < 0)
			fprintf(stderr, "ERR: bpf_set_link_xdp_fd(): %d %s\n",
				errno, strerror(errno));

		/* unlink pinned files */
		if (unlink(pin_prog_name))
			fprintf(stderr, "ERR: unlink(%s): %d %s\n",
				pin_prog_name, errno, strerror(errno));
		if (unlink(pin_vlanmap_name))
			fprintf(stderr, "ERR: unlink(%s): %d %s\n",
				pin_vlanmap_name, errno, strerror(errno));
		if (unlink(pin_countermap_name))
			fprintf(stderr, "ERR: unlink(%s): %d %s\n",
				pin_countermap_name, errno, strerror(errno));

		return 0;
	}

	if (vlan == 0) {
		fprintf(stderr, "ERR: required option --vlan missing\n");
		goto error;
	}

	if (cpu == MAX_CPUS && vlan > 0) {
		fprintf(stderr, "ERR: required option --cpu missing\n");
		goto error;
	}

	vfd = bpf_obj_get(pin_vlanmap_name);
	if (vfd < 0) {
		fprintf(stderr, "ERR: can't find pinned map %s: %d %s\n",
			pin_vlanmap_name, errno, strerror(errno));
		if (errno == ENOENT)
			fprintf(stderr, "   (has %s been installed yet?)\n", argv[0]);
		return -1;
	}

	/* decode the requested action */
	if (vlan > 0) {
		/* check cpu against the max value found */
		cfd = bpf_obj_get(pin_countermap_name);
		if (cfd < 0) {
			fprintf(stderr, "ERR: can't find pinned map %s: %d %s\n",
				pin_countermap_name, errno, strerror(errno));
			return -1;
		}
		c64 = CPU_COUNT;
		ret = bpf_map_lookup_elem(cfd, &c64, &v64);
		if (cpu >= v64) {
			fprintf(stderr, "ERR: cpu %d greater than max %llu\n", cpu, v64);
			return -1;
		}

		/* Note that the value and key pointers really do need to be
		 * pointers to 64-bit values, else things get a bit muddled.
		 */
		v64 = vlan;
		c64 = cpu;
		ret = bpf_map_update_elem(vfd, &v64, &c64, 0);
		if (ret) {
			fprintf(stderr, "Adding vlan %d CPU %d failed: %d %s\n",
				vlan, cpu, errno, strerror(errno));
			return -1;
		}

	} else {
		v64 = -vlan;
		c64 = UNDEF_CPU;

		/* We can't actually delete from a TYPE_ARRAY map, so we
		 * simply set it to an undefined value.
		 */
		ret = bpf_map_update_elem(vfd, &v64, &c64, 0);
		if (ret) {
			fprintf(stderr, "Delete of vlan %llu failed: %d %s\n",
				v64, errno, strerror(errno));
			return -1;
		}
	}

	return 0;

error:
	usage(argv);
	return -1;
}
