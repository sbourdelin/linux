#ifndef __NETNS_CORE_H__
#define __NETNS_CORE_H__

struct ctl_table_header;
struct prot_inuse;

struct netns_core {
	/* core sysctls */
	struct ctl_table_header	*sysctl_hdr;

	int	sysctl_somaxconn;
	u32	sysctl_wmem_max;
	u32	sysctl_rmem_max;

	u32	sysctl_wmem_default;
	u32	sysctl_rmem_default;

	struct prot_inuse __percpu *inuse;
};

#endif
