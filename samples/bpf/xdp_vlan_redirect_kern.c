// SPDX-License-Identifier: GPL-2.0
/*  XDP redirect vlans to CPUs - kernel code
 *
 *  Copyright(c) 2018 Oracle Corp.
 */

#include <linux/if_ether.h>
#include <linux/if_vlan.h>

#include <uapi/linux/bpf.h>
#include "bpf_helpers.h"

char _license[] SEC("license") = "GPL";

#define MAX_CPUS 64           /* WARNING - sync with _user.c */
#define UNDEF_CPU 0xff000000  /* WARNING - sync with _user.c */

/* The vlan index finds cpu(s) for processing a packet */
struct bpf_map_def SEC("maps") vlan_redirect_map = {
	.type = BPF_MAP_TYPE_ARRAY,
	.key_size = sizeof(u32),		/* vlan tag */
	.value_size = sizeof(u64),		/* cpu bitpattern */
	.max_entries = 4096,
};

/* List of cpus that can participate in the vlan redirect */
struct bpf_map_def SEC("maps") vlan_redirect_cpus_map = {
	.type		= BPF_MAP_TYPE_CPUMAP,
	.key_size	= sizeof(u32),		/* cpu id */
	.value_size	= sizeof(u32),		/* queue size */
	.max_entries	= MAX_CPUS,
};

/* Counters for debug */
#define VRC_CALLS  0    /* number of calls to this program */
#define VRC_VLANS  1    /* number of vlan packets seen */
#define VRC_HITS   2    /* number of redirects attempted */
#define CPU_COUNT  3    /* number of CPUs found */
struct bpf_map_def SEC("maps") vlan_redirect_counters_map = {
	.type = BPF_MAP_TYPE_ARRAY,
	.key_size = sizeof(u32),		/* vlan tag */
	.value_size = sizeof(u64),		/* cpu bitpattern */
	.max_entries = 4,
};

SEC("xdp_vlan_redirect")
int xdp_vlan_redirect(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data     = (void *)(long)ctx->data;
	struct ethhdr *eth = data;
	uint64_t h_proto, nh_off;
	struct vlan_hdr *vhdr;
	uint64_t *cpu_value;
	uint32_t num_cpus;
	uint32_t minlen;
	uint64_t *vp;
	uint64_t v, k;
	uint64_t vlan;

	/* count packets processed */
	k = VRC_CALLS;
	vp = bpf_map_lookup_elem(&vlan_redirect_counters_map, &k);
	if (vp) {
		v = (*vp) + 1;
		bpf_map_update_elem(&vlan_redirect_counters_map, &k, &v, 0);
	}

	/* is there enough packet? */
	minlen = sizeof(struct ethhdr) + sizeof(struct vlan_hdr);
	if (data + minlen > data_end)
		return XDP_PASS;

	/* is there a vlan tag? */
	h_proto = be16_to_cpu(eth->h_proto);
	if (h_proto != ETH_P_8021Q && h_proto != ETH_P_8021AD)
		return XDP_PASS;

	vhdr = data + sizeof(struct ethhdr);
	vlan = be16_to_cpu(vhdr->h_vlan_TCI) & VLAN_VID_MASK;
	if (!vlan)
		return XDP_PASS;

	/* count vlan packets seen */
	k = VRC_VLANS;
	vp = bpf_map_lookup_elem(&vlan_redirect_counters_map, &k);
	if (vp) {
		v = (*vp) + 1;
		bpf_map_update_elem(&vlan_redirect_counters_map, &k, &v, 0);
	}

	/* what cpu(s) for this vlanid? */
	cpu_value = bpf_map_lookup_elem(&vlan_redirect_map, &vlan);
	if (!cpu_value || *cpu_value == UNDEF_CPU)
		return XDP_PASS;

	/* TODO: cpu_value could be a bit-pattern of possible cpus
	 *       from which to choose, and here we would do a hash
	 *       to select the target cpu.
	 */

	/* TODO: scale the cpu found to be sure it is less than the
	 *       actual number of CPUs running.
	 */

	/* set up the redirect */
	bpf_redirect_map(&vlan_redirect_cpus_map, *cpu_value, 0);

	/* count redirects attempted */
	k = VRC_HITS;
	vp = bpf_map_lookup_elem(&vlan_redirect_counters_map, &k);
	if (vp) {
		v = (*vp) + 1;
		bpf_map_update_elem(&vlan_redirect_counters_map, &k, &v, 0);
	}

	return XDP_REDIRECT;
}
