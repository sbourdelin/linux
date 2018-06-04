// SPDX-License-Identifier: GPL-2.0
/* Octeon III Schedule/Synchronize/Order Unit (SSO)
 *
 * Copyright (C) 2018 Cavium, Inc.
 */

#include "octeon3.h"

/* Registers are accessed via xkphys. */
#define SSO_BASE		0x1670000000000ull
#define SSO_ADDR(node)		(SET_XKPHYS + NODE_OFFSET(node) + SSO_BASE)

#define SSO_AW_STATUS(n)	(SSO_ADDR(n) + 0x000010e0)
#define SSO_AW_CFG(n)		(SSO_ADDR(n) + 0x000010f0)
#define SSO_ERR0(n)		(SSO_ADDR(n) + 0x00001240)
#define SSO_TAQ_ADD(n)		(SSO_ADDR(n) + 0x000020e0)
#define SSO_XAQ_AURA(n)		(SSO_ADDR(n) + 0x00002100)

#define AQ_OFFSET(g)		((g) << 3)
#define AQ_ADDR(n, g)		(SSO_ADDR(n) + AQ_OFFSET(g))
#define SSO_XAQ_HEAD_PTR(n, g)	(AQ_ADDR(n, g) + 0x00080000)
#define SSO_XAQ_TAIL_PTR(n, g)	(AQ_ADDR(n, g) + 0x00090000)
#define SSO_XAQ_HEAD_NEXT(n, g)	(AQ_ADDR(n, g) + 0x000a0000)
#define SSO_XAQ_TAIL_NEXT(n, g)	(AQ_ADDR(n, g) + 0x000b0000)

#define GRP_OFFSET(grp)		((grp) << 16)
#define GRP_ADDR(n, g)		(SSO_ADDR(n)  + GRP_OFFSET(g))
#define SSO_GRP_TAQ_THR(n, g)	(GRP_ADDR(n, g) + 0x20000100)
#define SSO_GRP_PRI(n, g)	(GRP_ADDR(n, g) + 0x20000200)
#define SSO_GRP_INT(n, g)	(GRP_ADDR(n, g) + 0x20000400)
#define SSO_GRP_INT_THR(n, g)	(GRP_ADDR(n, g) + 0x20000500)
#define SSO_GRP_AQ_CNT(n, g)	(GRP_ADDR(n, g) + 0x20000700)

static int octeon3_sso_get_num_groups(void)
{
	if (OCTEON_IS_MODEL(OCTEON_CN78XX))
		return 256;
	if (OCTEON_IS_MODEL(OCTEON_CNF75XX) || OCTEON_IS_MODEL(OCTEON_CN73XX))
		return 64;
	return 0;
}

void octeon3_sso_irq_set(int node, int group, bool enable)
{
	if (enable)
		oct_csr_write(1, SSO_GRP_INT_THR(node, group));
	else
		oct_csr_write(0, SSO_GRP_INT_THR(node, group));

	oct_csr_write(BIT(1), SSO_GRP_INT(node, group));
}
EXPORT_SYMBOL(octeon3_sso_irq_set);

/* octeon3_sso_alloc_groups - Allocate a range of SSO groups.
 * @node: Node where SSO resides.
 * @groups: Pointer to allocated groups.
 * @cnt: Number of groups to allocate.
 * @start: Group number to start sequential allocation from. -1 for don't care.
 *
 * Returns 0 if successful, error code otherwise..
 */
int octeon3_sso_alloc_groups(int node, int *groups, int cnt, int start)
{
	struct global_resource_tag tag;
	int group, ret;
	char buf[16];

	strncpy((char *)&tag.lo, "cvm_sso_", 8);
	snprintf(buf, 16, "0%d......", node);
	memcpy(&tag.hi, buf, 8);

	res_mgr_create_resource(tag, octeon3_sso_get_num_groups());

	if (!groups)
		ret = res_mgr_alloc_range(tag, start, cnt, false, &group);
		if (!ret)
			ret = group;
	else
		ret = res_mgr_alloc_range(tag, start, cnt, false, groups);

	return ret;
}
EXPORT_SYMBOL(octeon3_sso_alloc_groups);

/* octeon3_sso_free_groups - Free SSO groups.
 * @node: Node where SSO resides.
 * @groups: Array of groups to free.
 * @cnt: Number of groups to free.
 */
void octeon3_sso_free_groups(int node, int *groups, int	cnt)
{
	struct global_resource_tag tag;
	char buf[16];

	/* Allocate the requested groups. */
	strncpy((char *)&tag.lo, "cvm_sso_", 8);
	snprintf(buf, 16, "0%d......", node);
	memcpy(&tag.hi, buf, 8);

	res_mgr_free_range(tag, groups, cnt);
}
EXPORT_SYMBOL(octeon3_sso_free_groups);

/* octeon3_sso_pass1_limit - When the Transitory Admission Queue (TAQ) is
 *   almost full, it is possible for the SSo to hang. We work around this
 *   by ensuring that the sum of SSO_GRP(0..255)_TAQ_THR[MAX_THR] of all
 *   used groups is <= 1264. This may reduce single group performance when
 *   many groups are in use.
 * @node: Node to update.
 * @grp: SSO group to update.
 */
void octeon3_sso_pass1_limit(int node, int group)
{
	u64 max_thr, rsvd_thr, taq_add, taq_thr;

	/* Ideally we would like to divide the maximum number of TAQ buffers
	 * (1264) among the SSO groups in use. However, since we do not know
	 * how many SSO groups are used by code outside this driver, we take
	 * the worst case approach.
	 */
	max_thr = 1264 / octeon3_sso_get_num_groups();
	if (max_thr < 4)
		max_thr = 4;
	rsvd_thr = max_thr - 1;

	/* Changes to SSO_GRP_TAQ_THR[rsvd_thr] must also update
	 * SSO_TAQ_ADD[RSVD_FREE].
	 */
	taq_thr = oct_csr_read(SSO_GRP_TAQ_THR(node, group));
	taq_add = (rsvd_thr - (taq_thr & GENMASK_ULL(10, 0))) << 16;

	taq_thr &= ~(GENMASK_ULL(42, 32) | GENMASK_ULL(10, 0));
	taq_thr |= max_thr << 32;
	taq_thr |= rsvd_thr;

	oct_csr_write(taq_thr, SSO_GRP_TAQ_THR(node, group));
	oct_csr_write(taq_add, SSO_TAQ_ADD(node));
}
EXPORT_SYMBOL(octeon3_sso_pass1_limit);

/* octeon3_sso_shutdown - Shutdown the SSO.
 * @node: Node where SSO to disable is.
 * @aura: Aura used for the SSO buffers.
 */
void octeon3_sso_shutdown(int node, int aura)
{
	int i, max_grps, timeout;
	u64 data, head, tail;
	void *ptr;

	/* Disable SSO. */
	data = oct_csr_read(SSO_AW_CFG(node));
	data |= BIT(6) | BIT(4);
	data &= ~BIT(0);
	oct_csr_write(data, SSO_AW_CFG(node));

	/* Extract the FPA buffers. */
	max_grps = octeon3_sso_get_num_groups();
	for (i = 0; i < max_grps; i++) {
		head = oct_csr_read(SSO_XAQ_HEAD_PTR(node, i));
		tail = oct_csr_read(SSO_XAQ_TAIL_PTR(node, i));
		data = oct_csr_read(SSO_GRP_AQ_CNT(node, i));

		/* Verify pointers. */
		head &= GENMASK_ULL(41, 7);
		tail &= GENMASK_ULL(41, 7);
		if (head != tail) {
			pr_err("%s: Bad pointer\n", __func__);
			continue;
		}

		/* This SSO group should have no pending entries. */
		if (data & GENMASK_ULL(32, 0))
			pr_err("%s: Group not empty\n", __func__);

		ptr = phys_to_virt(head);
		octeon_fpa3_free(node, aura, ptr);

		/* Clear pointers. */
		oct_csr_write(0, SSO_XAQ_HEAD_PTR(node, i));
		oct_csr_write(0, SSO_XAQ_HEAD_NEXT(node, i));
		oct_csr_write(0, SSO_XAQ_TAIL_PTR(node, i));
		oct_csr_write(0, SSO_XAQ_TAIL_NEXT(node, i));
	}

	/* Make sure all buffers are drained. */
	timeout = 10000;
	do {
		data = oct_csr_read(SSO_AW_STATUS(node));
		if ((data & GENMASK_ULL(5, 0)) == 0)
			break;
		timeout--;
		udelay(1);
	} while (timeout);
	if (!timeout)
		pr_err("%s: Timed out draining buffers\n", __func__);
}
EXPORT_SYMBOL(octeon3_sso_shutdown);

/* octeon3_sso_init - Initialize the SSO.
 * @node: Node where SSO resides.
 * @aura: Aura used for the SSO buffers.
 */
int octeon3_sso_init(int node, int aura)
{
	int i, max_grps;
	u64 data, phys;
	int err = 0;
	void *mem;

	data = BIT(3) | BIT(2) | BIT(1);
	oct_csr_write(data, SSO_AW_CFG(node));

	data = (node << 10) | aura;
	oct_csr_write(data, SSO_XAQ_AURA(node));

	max_grps = octeon3_sso_get_num_groups();
	for (i = 0; i < max_grps; i++) {
		mem = octeon_fpa3_alloc(node, aura);
		if (!mem) {
			err = -ENOMEM;
			goto out;
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
out:
	return err;
}
EXPORT_SYMBOL(octeon3_sso_init);
