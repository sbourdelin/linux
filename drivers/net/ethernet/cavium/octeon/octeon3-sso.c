/*
 * Copyright (c) 2017 Cavium, Inc.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/module.h>

#include <asm/octeon/octeon.h>

#include "octeon3.h"

/* Registers are accessed via xkphys */
#define SSO_BASE			0x1670000000000ull
#define SSO_ADDR(node)			(SET_XKPHYS + NODE_OFFSET(node) +      \
					 SSO_BASE)

#define SSO_AW_STATUS(n)		(SSO_ADDR(n)		   + 0x000010e0)
#define SSO_AW_CFG(n)			(SSO_ADDR(n)		   + 0x000010f0)
#define SSO_ERR0(n)			(SSO_ADDR(n)		   + 0x00001240)
#define SSO_TAQ_ADD(n)			(SSO_ADDR(n)		   + 0x000020e0)
#define SSO_XAQ_AURA(n)			(SSO_ADDR(n)		   + 0x00002100)

#define AQ_OFFSET(g)			((g) << 3)
#define AQ_ADDR(n, g)			(SSO_ADDR(n) + AQ_OFFSET(g))
#define SSO_XAQ_HEAD_PTR(n, g)		(AQ_ADDR(n, g)		   + 0x00080000)
#define SSO_XAQ_TAIL_PTR(n, g)		(AQ_ADDR(n, g)		   + 0x00090000)
#define SSO_XAQ_HEAD_NEXT(n, g)		(AQ_ADDR(n, g)		   + 0x000a0000)
#define SSO_XAQ_TAIL_NEXT(n, g)		(AQ_ADDR(n, g)		   + 0x000b0000)

#define GRP_OFFSET(grp)			((grp) << 16)
#define GRP_ADDR(n, g)			(SSO_ADDR(n) + GRP_OFFSET(g))
#define SSO_GRP_TAQ_THR(n, g)		(GRP_ADDR(n, g)		   + 0x20000100)
#define SSO_GRP_PRI(n, g)		(GRP_ADDR(n, g)		   + 0x20000200)
#define SSO_GRP_INT(n, g)		(GRP_ADDR(n, g)		   + 0x20000400)
#define SSO_GRP_INT_THR(n, g)		(GRP_ADDR(n, g)		   + 0x20000500)
#define SSO_GRP_AQ_CNT(n, g)		(GRP_ADDR(n, g)		   + 0x20000700)

static int get_num_sso_grps(void)
{
	if (OCTEON_IS_MODEL(OCTEON_CN78XX))
		return 256;
	if (OCTEON_IS_MODEL(OCTEON_CNF75XX) || OCTEON_IS_MODEL(OCTEON_CN73XX))
		return 64;
	return 0;
}

void octeon3_sso_irq_set(int node, int grp, bool en)
{
	if (en)
		oct_csr_write(1, SSO_GRP_INT_THR(node, grp));
	else
		oct_csr_write(0, SSO_GRP_INT_THR(node, grp));

	oct_csr_write(BIT(1), SSO_GRP_INT(node, grp));
}
EXPORT_SYMBOL(octeon3_sso_irq_set);

/**
 * octeon3_sso_alloc_grp_range - Allocate a range of sso groups.
 * @node: Node where sso resides.
 * @req_grp: Group number to start allocating sequentially from. -1 for don't
 *	     care.
 * @req_cnt: Number of groups to allocate.
 * @use_last_avail: Set to request the last available groups.
 * @grp: Updated with allocated groups.
 *
 * Returns 0 if successful.
 * Returns <0 for error codes.
 */
int octeon3_sso_alloc_grp_range(int	node,
				int	req_grp,
				int	req_cnt,
				bool	use_last_avail,
				int	*grp)
{
	struct global_resource_tag	tag;
	char				buf[16];

	/* Allocate the request group range */
	strncpy((char *)&tag.lo, "cvm_sso_", 8);
	snprintf(buf, 16, "0%d......", node);
	memcpy(&tag.hi, buf, 8);

	res_mgr_create_resource(tag, get_num_sso_grps());
	return res_mgr_alloc_range(tag, req_grp, req_cnt, false, grp);
}
EXPORT_SYMBOL(octeon3_sso_alloc_grp_range);

/**
 * octeon3_sso_alloc_grp - Allocate a sso group.
 * @node: Node where sso resides.
 * @req_grp: Group number to allocate, -1 for don't care.
 *
 * Returns allocated group.
 * Returns <0 for error codes.
 */
int octeon3_sso_alloc_grp(int node, int req_grp)
{
	int	grp;
	int	rc;

	rc = octeon3_sso_alloc_grp_range(node, req_grp, 1, false, &grp);
	if (!rc)
		rc = grp;

	return rc;
}
EXPORT_SYMBOL(octeon3_sso_alloc_grp);

/**
 * octeon3_sso_free_grp_range - Free a range of sso groups.
 * @node: Node where sso resides.
 * @grp: Array of groups to free.
 * @req_cnt: Number of groups to free.
 */
void octeon3_sso_free_grp_range(int	node,
				int	*grp,
				int	req_cnt)
{
	struct global_resource_tag	tag;
	char				buf[16];

	/* Allocate the request group range */
	strncpy((char *)&tag.lo, "cvm_sso_", 8);
	snprintf(buf, 16, "0%d......", node);
	memcpy(&tag.hi, buf, 8);

	res_mgr_free_range(tag, grp, req_cnt);
}
EXPORT_SYMBOL(octeon3_sso_free_grp_range);

/**
 * octeon3_sso_free_grp - Free a sso group.
 * @node: Node where sso resides.
 * @grp: Group to free.
 */
void octeon3_sso_free_grp(int	node,
			  int	grp)
{
	octeon3_sso_free_grp_range(node, &grp, 1);
}
EXPORT_SYMBOL(octeon3_sso_free_grp);

/**
 * octeon3_sso_pass1_limit - Near full TAQ can cause hang. When the TAQ
 *			     (Transitory Admission Queue) is near-full, it is
 *			     possible for SSO to hang.
 *			     Workaround: Ensure that the sum of
 *			     SSO_GRP(0..255)_TAQ_THR[MAX_THR] of all used
 *			     groups is <= 1264. This may reduce single-group
 *			     performance when many groups are used.
 *
 * @node: Node to update.
 * @grp: SSO group to update.
 */
void octeon3_sso_pass1_limit(int node, int grp)
{
	u64	taq_thr;
	u64	taq_add;
	u64	max_thr;
	u64	rsvd_thr;

	/* Ideally, we would like to divide the maximum number of TAQ buffers
	 * (1264) among the sso groups in use. However, since we don't know how
	 * many sso groups are used by code outside this driver we take the
	 * worst case approach and assume all 256 sso groups must be supported.
	 */
	max_thr = 1264 / get_num_sso_grps();
	if (max_thr < 4)
		max_thr = 4;
	rsvd_thr = max_thr - 1;

	/* Changes to SSO_GRP_TAQ_THR[rsvd_thr] must also update
	 * SSO_TAQ_ADD[RSVD_FREE].
	 */
	taq_thr = oct_csr_read(SSO_GRP_TAQ_THR(node, grp));
	taq_add = (rsvd_thr - (taq_thr & GENMASK_ULL(10, 0))) << 16;

	taq_thr &= ~(GENMASK_ULL(42, 32) | GENMASK_ULL(10, 0));
	taq_thr |= max_thr << 32;
	taq_thr |= rsvd_thr;

	oct_csr_write(taq_thr, SSO_GRP_TAQ_THR(node, grp));
	oct_csr_write(taq_add, SSO_TAQ_ADD(node));
}
EXPORT_SYMBOL(octeon3_sso_pass1_limit);

/**
 * octeon3_sso_shutdown - Shutdown the sso. It undoes what octeon3_sso_init()
 *			  did.
 * @node: Node where sso to disable is.
 * @aura: Aura used for the sso buffers.
 */
void octeon3_sso_shutdown(int node, int aura)
{
	u64	data;
	int	max_grps;
	int	timeout;
	int	i;

	/* Disable sso */
	data = oct_csr_read(SSO_AW_CFG(node));
	data |= BIT(6) | BIT(4);
	data &= ~BIT(0);
	oct_csr_write(data, SSO_AW_CFG(node));

	/* Extract the fpa buffers */
	max_grps = get_num_sso_grps();
	for (i = 0; i < max_grps; i++) {
		u64	head;
		u64	tail;
		void	*ptr;

		head = oct_csr_read(SSO_XAQ_HEAD_PTR(node, i));
		tail = oct_csr_read(SSO_XAQ_TAIL_PTR(node, i));
		data = oct_csr_read(SSO_GRP_AQ_CNT(node, i));

		/* Verify pointers */
		head &= GENMASK_ULL(41, 7);
		tail &= GENMASK_ULL(41, 7);
		if (head != tail) {
			pr_err("octeon3_sso: bad ptr\n");
			continue;
		}

		/* This sso group should have no pending entries */
		if (data & GENMASK_ULL(32, 0))
			pr_err("octeon3_sso: not empty\n");

		ptr = phys_to_virt(head);
		octeon_fpa3_free(node, aura, ptr);

		/* Clear pointers */
		oct_csr_write(0, SSO_XAQ_HEAD_PTR(node, i));
		oct_csr_write(0, SSO_XAQ_HEAD_NEXT(node, i));
		oct_csr_write(0, SSO_XAQ_TAIL_PTR(node, i));
		oct_csr_write(0, SSO_XAQ_TAIL_NEXT(node, i));
	}

	/* Make sure all buffers drained */
	timeout = 10000;
	do {
		data = oct_csr_read(SSO_AW_STATUS(node));
		if ((data & GENMASK_ULL(5, 0)) == 0)
			break;
		timeout--;
		udelay(1);
	} while (timeout);
	if (!timeout)
		pr_err("octeon3_sso: timeout\n");
}
EXPORT_SYMBOL(octeon3_sso_shutdown);

/**
 * octeon3_sso_init - Initialize the sso.
 * @node: Node where sso resides.
 * @aura: Aura used for the sso buffers.
 */
int octeon3_sso_init(int node, int aura)
{
	u64	data;
	int	max_grps;
	int	i;
	int	rc = 0;

	data = BIT(3) | BIT(2) | BIT(1);
	oct_csr_write(data, SSO_AW_CFG(node));

	data = (node << 10) | aura;
	oct_csr_write(data, SSO_XAQ_AURA(node));

	max_grps = get_num_sso_grps();
	for (i = 0; i < max_grps; i++) {
		u64	phys;
		void	*mem;

		mem = octeon_fpa3_alloc(node, aura);
		if (!mem) {
			rc = -ENOMEM;
			goto err;
		}

		phys = virt_to_phys(mem);
		oct_csr_write(phys, SSO_XAQ_HEAD_PTR(node, i));
		oct_csr_write(phys, SSO_XAQ_HEAD_NEXT(node, i));
		oct_csr_write(phys, SSO_XAQ_TAIL_PTR(node, i));
		oct_csr_write(phys, SSO_XAQ_TAIL_NEXT(node, i));

		/* SSO-18678 */
		data = 0x3f << 16;
		oct_csr_write(data, SSO_GRP_PRI(node, i));
	}

	data = BIT(0);
	oct_csr_write(data, SSO_ERR0(node));

	data = BIT(3) | BIT(2) | BIT(1) | BIT(0);
	oct_csr_write(data, SSO_AW_CFG(node));

 err:
	return rc;
}
EXPORT_SYMBOL(octeon3_sso_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cavium, Inc. <support@cavium.com>");
MODULE_DESCRIPTION("Cavium, Inc. SSO management.");
