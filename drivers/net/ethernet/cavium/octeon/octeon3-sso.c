// SPDX-License-Identifier: GPL-2.0
/* Octeon III Schedule/Synchronize/Order Unit (SSO)
 *
 * Copyright (C) 2018 Cavium, Inc.
 */

#include "octeon3.h"

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

	oct_csr_write(SSO_GRP_INT_EXE_INT, SSO_GRP_INT(node, group));
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
	taq_add = (rsvd_thr - (taq_thr & SSO_GRP_TAQ_THR_RSVD_THR_MASK)) <<
		  SSO_TAQ_ADD_RSVD_FREE_SHIFT;

	taq_thr &= ~(SSO_GRP_TAQ_THR_MAX_THR_MASK |
		     SSO_GRP_TAQ_THR_RSVD_THR_MASK);
	taq_thr |= max_thr << SSO_GRP_TAQ_THR_RSVD_THR_SHIFT;
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
	data |= SSO_AW_CFG_XAQ_ALOC_DIS | SSO_AW_CFG_XAQ_BYP_DIS;
	data &= ~SSO_AW_CFG_RWEN;
	oct_csr_write(data, SSO_AW_CFG(node));

	/* Extract the FPA buffers. */
	max_grps = octeon3_sso_get_num_groups();
	for (i = 0; i < max_grps; i++) {
		head = oct_csr_read(SSO_XAQ_HEAD_PTR(node, i));
		tail = oct_csr_read(SSO_XAQ_TAIL_PTR(node, i));
		data = oct_csr_read(SSO_GRP_AQ_CNT(node, i));

		/* Verify pointers. */
		head &= SSO_XAQ_PTR_MASK;
		tail &= SSO_XAQ_PTR_MASK;
		if (head != tail) {
			pr_err("%s: Bad pointer\n", __func__);
			continue;
		}

		/* This SSO group should have no pending entries. */
		if (data & SSO_GRP_AQ_CNT_AQ_CNT_MASK)
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
		if ((data & SSO_AW_STATUS_XAQ_BU_CACHED_MASK) == 0)
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
	int i, max_grps, err = 0;
	u64 data, phys;
	void *mem;

	data = SSO_AW_CFG_STT | SSO_AW_CFG_LDT | SSO_AW_CFG_LDWB;
	oct_csr_write(data, SSO_AW_CFG(node));

	data = (node << SSO_XAQ_AURA_NODE_SHIFT) | aura;
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
		data = SSO_GRP_PRI_WEIGHT_MAXIMUM << SSO_GRP_PRI_WEIGHT_SHIFT;
		oct_csr_write(data, SSO_GRP_PRI(node, i));
	}

	data = SSO_ERR0_FPE;
	oct_csr_write(data, SSO_ERR0(node));

	data = SSO_AW_CFG_STT | SSO_AW_CFG_LDT | SSO_AW_CFG_LDWB |
	       SSO_AW_CFG_RWEN;
	oct_csr_write(data, SSO_AW_CFG(node));
out:
	return err;
}
EXPORT_SYMBOL(octeon3_sso_init);
