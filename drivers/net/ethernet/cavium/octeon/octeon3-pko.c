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

#define MAX_OUTPUT_MAC			28
#define MAX_FIFO_GRP			8

#define FIFO_SIZE			2560

/* Registers are accessed via xkphys */
#define PKO_BASE			0x1540000000000ull
#define PKO_ADDR(node)			(SET_XKPHYS + NODE_OFFSET(node) +      \
					 PKO_BASE)

#define PKO_L1_SQ_SHAPE(n, q)		(PKO_ADDR(n) + ((q) << 9)    + 0x000010)
#define PKO_L1_SQ_LINK(n, q)		(PKO_ADDR(n) + ((q) << 9)    + 0x000038)
#define PKO_DQ_WM_CTL(n, q)		(PKO_ADDR(n) + ((q) << 9)    + 0x000040)
#define PKO_L1_SQ_TOPOLOGY(n, q)	(PKO_ADDR(n) + ((q) << 9)    + 0x080000)
#define PKO_L2_SQ_SCHEDULE(n, q)	(PKO_ADDR(n) + ((q) << 9)    + 0x080008)
#define PKO_L3_L2_SQ_CHANNEL(n, q)	(PKO_ADDR(n) + ((q) << 9)    + 0x080038)
#define PKO_CHANNEL_LEVEL(n)		(PKO_ADDR(n)		     + 0x0800f0)
#define PKO_SHAPER_CFG(n)		(PKO_ADDR(n)		     + 0x0800f8)
#define PKO_L2_SQ_TOPOLOGY(n, q)	(PKO_ADDR(n) + ((q) << 9)    + 0x100000)
#define PKO_L3_SQ_SCHEDULE(n, q)	(PKO_ADDR(n) + ((q) << 9)    + 0x100008)
#define PKO_L3_SQ_TOPOLOGY(n, q)	(PKO_ADDR(n) + ((q) << 9)    + 0x180000)
#define PKO_L4_SQ_SCHEDULE(n, q)	(PKO_ADDR(n) + ((q) << 9)    + 0x180008)
#define PKO_L4_SQ_TOPOLOGY(n, q)	(PKO_ADDR(n) + ((q) << 9)    + 0x200000)
#define PKO_L5_SQ_SCHEDULE(n, q)	(PKO_ADDR(n) + ((q) << 9)    + 0x200008)
#define PKO_L5_SQ_TOPOLOGY(n, q)	(PKO_ADDR(n) + ((q) << 9)    + 0x280000)
#define PKO_DQ_SCHEDULE(n, q)		(PKO_ADDR(n) + ((q) << 9)    + 0x280008)
#define PKO_DQ_SW_XOFF(n, q)		(PKO_ADDR(n) + ((q) << 9)    + 0x2800e0)
#define PKO_DQ_TOPOLOGY(n, q)		(PKO_ADDR(n) + ((q) << 9)    + 0x300000)
#define PKO_PDM_CFG(n)			(PKO_ADDR(n)		     + 0x800000)
#define PKO_PDM_DQ_MINPAD(n, q)		(PKO_ADDR(n) + ((q) << 3)    + 0x8f0000)
#define PKO_MAC_CFG(n, m)		(PKO_ADDR(n) + ((m) << 3)    + 0x900000)
#define PKO_PTF_STATUS(n, f)		(PKO_ADDR(n) + ((f) << 3)    + 0x900100)
#define PKO_PTGF_CFG(n, g)		(PKO_ADDR(n) + ((g) << 3)    + 0x900200)
#define PKO_PTF_IOBP_CFG(n)		(PKO_ADDR(n)		     + 0x900300)
#define PKO_MCI0_MAX_CRED(n, m)		(PKO_ADDR(n) + ((m) << 3)    + 0xa00000)
#define PKO_MCI1_MAX_CRED(n, m)		(PKO_ADDR(n) + ((m) << 3)    + 0xa80000)
#define PKO_LUT(n, c)			(PKO_ADDR(n) + ((c) << 3)    + 0xb00000)
#define PKO_DPFI_STATUS(n)		(PKO_ADDR(n)		     + 0xc00000)
#define PKO_DPFI_FLUSH(n)		(PKO_ADDR(n)		     + 0xc00008)
#define PKO_DPFI_FPA_AURA(n)		(PKO_ADDR(n)		     + 0xc00010)
#define PKO_DPFI_ENA(n)			(PKO_ADDR(n)		     + 0xc00018)
#define PKO_STATUS(n)			(PKO_ADDR(n)		     + 0xd00000)
#define PKO_ENABLE(n)			(PKO_ADDR(n)		     + 0xd00008)

/* These levels mimic the pko internal linked queue structure */
enum queue_level {
	PQ	= 1,
	L2_SQ	= 2,
	L3_SQ	= 3,
	L4_SQ	= 4,
	L5_SQ	= 5,
	DQ	= 6
};

enum pko_dqop_e {
	DQOP_SEND,
	DQOP_OPEN,
	DQOP_CLOSE,
	DQOP_QUERY
};

enum pko_dqstatus_e {
	PASS = 0,
	BADSTATE = 0x8,
	NOFPABUF = 0x9,
	NOPKOBUF = 0xa,
	FAILRTNPTR = 0xb,
	ALREADY = 0xc,
	NOTCREATED = 0xd,
	NOTEMPTY = 0xe,
	SENDPKTDROP = 0xf
};

struct mac_info {
	int	fifo_cnt;
	int	prio;
	int	speed;
	int	fifo;
	int	num_lmacs;
};

struct fifo_grp_info {
	int	speed;
	int	size;
};

static const int lut_index_78xx[] = {
	0x200,
	0x240,
	0x280,
	0x2c0,
	0x300,
	0x340
};

static const int lut_index_73xx[] = {
	0x000,
	0x040,
	0x080
};

static enum queue_level max_sq_level(void)
{
	/* 73xx and 75xx only have 3 scheduler queue levels */
	if (OCTEON_IS_MODEL(OCTEON_CN73XX) || OCTEON_IS_MODEL(OCTEON_CNF75XX))
		return L3_SQ;

	return L5_SQ;
}

static int get_num_fifos(void)
{
	if (OCTEON_IS_MODEL(OCTEON_CN73XX) || OCTEON_IS_MODEL(OCTEON_CNF75XX))
		return 16;

	return 28;
}

static int get_num_fifo_groups(void)
{
	if (OCTEON_IS_MODEL(OCTEON_CN73XX) || OCTEON_IS_MODEL(OCTEON_CNF75XX))
		return 5;

	return 8;
}

static int get_num_output_macs(void)
{
	if (OCTEON_IS_MODEL(OCTEON_CN78XX))
		return 28;
	else if (OCTEON_IS_MODEL(OCTEON_CNF75XX))
		return 10;
	else if (OCTEON_IS_MODEL(OCTEON_CN73XX))
		return 14;

	return 0;
}

static int get_output_mac(int			interface,
			  int			index,
			  enum octeon3_mac_type	mac_type)
{
	int mac;

	/* Output macs are hardcoded in the hardware. See PKO Output MACs
	 * section in the HRM.
	 */
	if (OCTEON_IS_MODEL(OCTEON_CN73XX) || OCTEON_IS_MODEL(OCTEON_CNF75XX)) {
		if (mac_type == SRIO_MAC)
			mac = 4 + 2 * interface + index;
		else
			mac = 2 + 4 * interface + index;
	} else {
		mac = 4 + 4 * interface + index;
	}

	return mac;
}

static int get_num_port_queues(void)
{
	if (OCTEON_IS_MODEL(OCTEON_CN73XX) || OCTEON_IS_MODEL(OCTEON_CNF75XX))
		return 16;

	return 32;
}

static int allocate_queues(int			node,
			   enum queue_level	level,
			   int			num_queues,
			   int			*queues)
{
	struct global_resource_tag	tag;
	char				buf[16];
	int				max_queues = 0;
	int				rc;

	if (level == PQ) {
		strncpy((char *)&tag.lo, "cvm_pkop", 8);
		snprintf(buf, 16, "oq_%d....", node);
		memcpy(&tag.hi, buf, 8);

		if (OCTEON_IS_MODEL(OCTEON_CN78XX))
			max_queues = 32;
		else
			max_queues = 16;
	} else if (level == L2_SQ) {
		strncpy((char *)&tag.lo, "cvm_pkol", 8);
		snprintf(buf, 16, "2q_%d....", node);
		memcpy(&tag.hi, buf, 8);

		if (OCTEON_IS_MODEL(OCTEON_CN78XX))
			max_queues = 512;
		else
			max_queues = 256;
	} else if (level == L3_SQ) {
		strncpy((char *)&tag.lo, "cvm_pkol", 8);
		snprintf(buf, 16, "3q_%d....", node);
		memcpy(&tag.hi, buf, 8);

		if (OCTEON_IS_MODEL(OCTEON_CN78XX))
			max_queues = 512;
		else
			max_queues = 256;
	} else if (level == L4_SQ) {
		strncpy((char *)&tag.lo, "cvm_pkol", 8);
		snprintf(buf, 16, "4q_%d....", node);
		memcpy(&tag.hi, buf, 8);

		if (OCTEON_IS_MODEL(OCTEON_CN78XX))
			max_queues = 1024;
		else
			max_queues = 0;
	} else if (level == L5_SQ) {
		strncpy((char *)&tag.lo, "cvm_pkol", 8);
		snprintf(buf, 16, "5q_%d....", node);
		memcpy(&tag.hi, buf, 8);

		if (OCTEON_IS_MODEL(OCTEON_CN78XX))
			max_queues = 1024;
		else
			max_queues = 0;
	} else if (level == DQ) {
		strncpy((char *)&tag.lo, "cvm_pkod", 8);
		snprintf(buf, 16, "eq_%d....", node);
		memcpy(&tag.hi, buf, 8);

		if (OCTEON_IS_MODEL(OCTEON_CN78XX))
			max_queues = 1024;
		else
			max_queues = 256;
	}

	res_mgr_create_resource(tag, max_queues);
	rc = res_mgr_alloc_range(tag, -1, num_queues, false, queues);
	if (rc < 0)
		return rc;

	return 0;
}

static void free_queues(int			node,
			enum queue_level	level,
			int			num_queues,
			const int		*queues)
{
	struct global_resource_tag	tag;
	char				buf[16];

	if (level == PQ) {
		strncpy((char *)&tag.lo, "cvm_pkop", 8);
		snprintf(buf, 16, "oq_%d....", node);
		memcpy(&tag.hi, buf, 8);
	} else if (level == L2_SQ) {
		strncpy((char *)&tag.lo, "cvm_pkol", 8);
		snprintf(buf, 16, "2q_%d....", node);
		memcpy(&tag.hi, buf, 8);
	} else if (level == L3_SQ) {
		strncpy((char *)&tag.lo, "cvm_pkol", 8);
		snprintf(buf, 16, "3q_%d....", node);
		memcpy(&tag.hi, buf, 8);
	} else if (level == L4_SQ) {
		strncpy((char *)&tag.lo, "cvm_pkol", 8);
		snprintf(buf, 16, "4q_%d....", node);
		memcpy(&tag.hi, buf, 8);
	} else if (level == L5_SQ) {
		strncpy((char *)&tag.lo, "cvm_pkol", 8);
		snprintf(buf, 16, "5q_%d....", node);
		memcpy(&tag.hi, buf, 8);
	} else if (level == DQ) {
		strncpy((char *)&tag.lo, "cvm_pkod", 8);
		snprintf(buf, 16, "eq_%d....", node);
		memcpy(&tag.hi, buf, 8);
	}

	res_mgr_free_range(tag, queues, num_queues);
}

static int port_queue_init(int	node,
			   int	pq,
			   int	mac)
{
	u64	data;

	data = mac << 16;
	oct_csr_write(data, PKO_L1_SQ_TOPOLOGY(node, pq));

	data = mac << 13;
	oct_csr_write(data, PKO_L1_SQ_SHAPE(node, pq));

	data = mac;
	data <<= 44;
	oct_csr_write(data, PKO_L1_SQ_LINK(node, pq));

	return 0;
}

static int scheduler_queue_l2_init(int	node,
				   int	queue,
				   int	parent_q)
{
	u64	data;

	data = oct_csr_read(PKO_L1_SQ_TOPOLOGY(node, parent_q));
	data &= ~(GENMASK_ULL(40, 32) | GENMASK_ULL(4, 1));
	data |= (u64)queue << 32;
	data |= 0xf << 1;
	oct_csr_write(data, PKO_L1_SQ_TOPOLOGY(node, parent_q));

	oct_csr_write(0, PKO_L2_SQ_SCHEDULE(node, queue));

	data = parent_q << 16;
	oct_csr_write(data, PKO_L2_SQ_TOPOLOGY(node, queue));

	return 0;
}

static int scheduler_queue_l3_init(int	node,
				   int	queue,
				   int	parent_q)
{
	u64	data;

	data = oct_csr_read(PKO_L2_SQ_TOPOLOGY(node, parent_q));
	data &= ~(GENMASK_ULL(40, 32) | GENMASK_ULL(4, 1));
	data |= (u64)queue << 32;
	data |= 0xf << 1;
	oct_csr_write(data, PKO_L2_SQ_TOPOLOGY(node, parent_q));

	oct_csr_write(0, PKO_L3_SQ_SCHEDULE(node, queue));

	data = parent_q << 16;
	oct_csr_write(data, PKO_L3_SQ_TOPOLOGY(node, queue));

	return 0;
}

static int scheduler_queue_l4_init(int	node,
				   int	queue,
				   int	parent_q)
{
	u64	data;

	data = oct_csr_read(PKO_L3_SQ_TOPOLOGY(node, parent_q));
	data &= ~(GENMASK_ULL(41, 32) | GENMASK_ULL(4, 1));
	data |= (u64)queue << 32;
	data |= 0xf << 1;
	oct_csr_write(data, PKO_L3_SQ_TOPOLOGY(node, parent_q));

	oct_csr_write(0, PKO_L4_SQ_SCHEDULE(node, queue));

	data = parent_q << 16;
	oct_csr_write(data, PKO_L4_SQ_TOPOLOGY(node, queue));

	return 0;
}

static int scheduler_queue_l5_init(int	node,
				   int	queue,
				   int	parent_q)
{
	u64	data;

	data = oct_csr_read(PKO_L4_SQ_TOPOLOGY(node, parent_q));
	data &= ~(GENMASK_ULL(41, 32) | GENMASK_ULL(4, 1));
	data |= (u64)queue << 32;
	data |= 0xf << 1;
	oct_csr_write(data, PKO_L4_SQ_TOPOLOGY(node, parent_q));

	oct_csr_write(0, PKO_L5_SQ_SCHEDULE(node, queue));

	data = parent_q << 16;
	oct_csr_write(data, PKO_L5_SQ_TOPOLOGY(node, queue));

	return 0;
}

static int descriptor_queue_init(int		node,
				 const int	*queue,
				 int		parent_q,
				 int		num_dq)
{
	u64	data;
	u64	addr;
	int	prio;
	int	rr_prio;
	int	rr_quantum;
	int	i;

	/* Limit static priorities to the available prio field bits */
	if (num_dq > 9) {
		pr_err("octeon3-pko: Invalid number of dqs\n");
		return -1;
	}

	prio = 0;

	if (num_dq == 1) {
		/* Single dq */
		rr_prio = 0xf;
		rr_quantum = 0x10;
	} else {
		/* Multiple dqs */
		rr_prio = num_dq;
		rr_quantum = 0;
	}

	if (OCTEON_IS_MODEL(OCTEON_CN78XX))
		addr = PKO_L5_SQ_TOPOLOGY(node, parent_q);
	else
		addr = PKO_L3_SQ_TOPOLOGY(node, parent_q);

	data = oct_csr_read(addr);
	data &= ~(GENMASK_ULL(41, 32) | GENMASK_ULL(4, 1));
	data |= (u64)queue[0] << 32;
	data |= rr_prio << 1;
	oct_csr_write(data, addr);

	for (i = 0; i < num_dq; i++) {
		data = (prio << 24) | rr_quantum;
		oct_csr_write(data, PKO_DQ_SCHEDULE(node, queue[i]));

		data = parent_q << 16;
		oct_csr_write(data, PKO_DQ_TOPOLOGY(node, queue[i]));

		data = BIT(49);
		oct_csr_write(data, PKO_DQ_WM_CTL(node, queue[i]));

		if (prio << rr_prio)
			prio++;
	}

	return 0;
}

static int map_channel(int	node,
		       int	pq,
		       int	queue,
		       int	ipd_port)
{
	u64	data;
	int	lut_index = 0;
	int	table_index;

	data = oct_csr_read(PKO_L3_L2_SQ_CHANNEL(node, queue));
	data &= ~GENMASK_ULL(43, 32);
	data |= (u64)ipd_port << 32;
	oct_csr_write(data, PKO_L3_L2_SQ_CHANNEL(node, queue));

	/* See PKO_LUT register description in the HRM for how to compose the
	 * lut_index.
	 */
	if (OCTEON_IS_MODEL(OCTEON_CN78XX)) {
		table_index = ((ipd_port & 0xf00) - 0x800) >> 8;
		lut_index = lut_index_78xx[table_index];
		lut_index += ipd_port & 0xff;
	} else if (OCTEON_IS_MODEL(OCTEON_CN73XX)) {
		table_index = ((ipd_port & 0xf00) - 0x800) >> 8;
		lut_index = lut_index_73xx[table_index];
		lut_index += ipd_port & 0xff;
	} else if (OCTEON_IS_MODEL(OCTEON_CNF75XX)) {
		if ((ipd_port & 0xf00) != 0x800)
			return -1;
		lut_index = ipd_port & 0xff;
	}

	data = BIT(15);
	data |= pq << 9;
	data |= queue;
	oct_csr_write(data, PKO_LUT(node, lut_index));

	return 0;
}

static int open_dq(int node, int dq)
{
	u64			data;
	u64			*iobdma_addr;
	u64			*scratch_addr;
	enum pko_dqstatus_e	status;

	/* Build the dq open query. See PKO_QUERY_DMA_S in the HRM for the
	 * query format.
	 */
	data = (LMTDMA_SCR_OFFSET >> 3) << 56;
	data |= 1ull << 48;
	data |= 0x51ull << 40;
	data |= (u64)node << 36;
	data |= (u64)DQOP_OPEN << 32;
	data |= dq << 16;

	CVMX_SYNCWS;
	preempt_disable();

	/* Clear return location */
	scratch_addr = (u64 *)(SCRATCH_BASE + LMTDMA_SCR_OFFSET);
	*scratch_addr = ~0ull;

	/* Issue pko lmtdma command */
	iobdma_addr = (u64 *)(IOBDMA_ORDERED_IO_ADDR);
	*iobdma_addr = data;

	/* Wait for lmtdma command to complete and get response*/
	CVMX_SYNCIOBDMA;
	data = *scratch_addr;

	preempt_enable();

	/* See PKO_QUERY_RTN_S in the HRM for response format */
	status = (data & GENMASK_ULL(63, 60)) >> 60;
	if (status != PASS && status != ALREADY) {
		pr_err("octeon3-pko: Failed to open dq\n");
		return -1;
	}

	return 0;
}

static s64 query_dq(int node, int dq)
{
	u64			data;
	u64			*iobdma_addr;
	u64			*scratch_addr;
	enum pko_dqstatus_e	status;
	s64			depth;

	/* Build the dq open query. See PKO_QUERY_DMA_S in the HRM for the
	 * query format.
	 */
	data = (LMTDMA_SCR_OFFSET >> 3) << 56;
	data |= 1ull << 48;
	data |= 0x51ull << 40;
	data |= (u64)node << 36;
	data |= (u64)DQOP_QUERY << 32;
	data |= dq << 16;

	CVMX_SYNCWS;
	preempt_disable();

	/* Clear return location */
	scratch_addr = (u64 *)(SCRATCH_BASE + LMTDMA_SCR_OFFSET);
	*scratch_addr = ~0ull;

	/* Issue pko lmtdma command */
	iobdma_addr = (u64 *)(IOBDMA_ORDERED_IO_ADDR);
	*iobdma_addr = data;

	/* Wait for lmtdma command to complete and get response*/
	CVMX_SYNCIOBDMA;
	data = *scratch_addr;

	preempt_enable();

	/* See PKO_QUERY_RTN_S in the HRM for response format */
	status = (data & GENMASK_ULL(63, 60)) >> 60;
	if (status != PASS) {
		pr_err("octeon3-pko: Failed to query dq=%d\n", dq);
		return -1;
	}

	depth = data & GENMASK_ULL(47, 0);

	return depth;
}

static u64 close_dq(int node, int dq)
{
	u64			data;
	u64			*iobdma_addr;
	u64			*scratch_addr;
	enum pko_dqstatus_e	status;

	/* Build the dq open query. See PKO_QUERY_DMA_S in the HRM for the
	 * query format.
	 */
	data = (LMTDMA_SCR_OFFSET >> 3) << 56;
	data |= 1ull << 48;
	data |= 0x51ull << 40;
	data |= (u64)node << 36;
	data |= (u64)DQOP_CLOSE << 32;
	data |= dq << 16;

	CVMX_SYNCWS;
	preempt_disable();

	/* Clear return location */
	scratch_addr = (u64 *)(SCRATCH_BASE + LMTDMA_SCR_OFFSET);
	*scratch_addr = ~0ull;

	/* Issue pko lmtdma command */
	iobdma_addr = (u64 *)(IOBDMA_ORDERED_IO_ADDR);
	*iobdma_addr = data;

	/* Wait for lmtdma command to complete and get response*/
	CVMX_SYNCIOBDMA;
	data = *scratch_addr;

	preempt_enable();

	/* See PKO_QUERY_RTN_S in the HRM for response format */
	status = (data & GENMASK_ULL(63, 60)) >> 60;
	if (status != PASS) {
		pr_err("octeon3-pko: Failed to close dq\n");
		return -1;
	}

	return 0;
}

static int get_78xx_fifos_required(int node, struct mac_info *macs)
{
	int		fifo_cnt = 0;
	int		bgx;
	int		index;
	int		qlm;
	int		num_lmacs;
	enum port_mode	mode;
	int		i;
	int		cnt;
	int		prio;
	u64		data;

	/* The loopback mac gets 1 fifo by default */
	macs[0].fifo_cnt = 1;
	macs[0].speed = 1;
	fifo_cnt += 1;

	/* The dpi mac gets 1 fifo by default */
	macs[1].fifo_cnt = 1;
	macs[1].speed = 50;
	fifo_cnt += 1;

	/* The ilk macs get default number of fifos (module param) */
	macs[2].fifo_cnt = ilk0_lanes <= 4 ? ilk0_lanes : 4;
	macs[2].speed = 40;
	fifo_cnt += macs[2].fifo_cnt;
	macs[3].fifo_cnt = ilk1_lanes <= 4 ? ilk1_lanes : 4;
	macs[3].speed = 40;
	fifo_cnt += macs[3].fifo_cnt;

	/* Assign fifos to the active bgx macs */
	for (i = 4; i < get_num_output_macs(); i += 4) {
		bgx = (i - 4) / 4;
		qlm = bgx_port_get_qlm(node, bgx, 0);

		data = oct_csr_read(GSER_CFG(node, qlm));
		if (data & BIT(2)) {
			data = oct_csr_read(BGX_CMR_TX_LMACS(node, bgx));
			num_lmacs = data & 7;

			for (index = 0; index < num_lmacs; index++) {
				switch (num_lmacs) {
				case 1:
					macs[i + index].num_lmacs = 4;
					break;
				case 2:
					macs[i + index].num_lmacs = 2;
					break;
				case 4:
				default:
					macs[i + index].num_lmacs = 1;
					break;
				}

				mode = bgx_port_get_mode(node, bgx, 0);
				switch (mode) {
				case PORT_MODE_SGMII:
				case PORT_MODE_RGMII:
					macs[i + index].fifo_cnt = 1;
					macs[i + index].prio = 1;
					macs[i + index].speed = 1;
					break;

				case PORT_MODE_XAUI:
				case PORT_MODE_RXAUI:
					macs[i + index].fifo_cnt = 4;
					macs[i + index].prio = 2;
					macs[i + index].speed = 20;
					break;

				case PORT_MODE_10G_KR:
				case PORT_MODE_XFI:
					macs[i + index].fifo_cnt = 4;
					macs[i + index].prio = 2;
					macs[i + index].speed = 10;
					break;

				case PORT_MODE_40G_KR4:
				case PORT_MODE_XLAUI:
					macs[i + index].fifo_cnt = 4;
					macs[i + index].prio = 3;
					macs[i + index].speed = 40;
					break;

				default:
					macs[i + index].fifo_cnt = 0;
					macs[i + index].prio = 0;
					macs[i + index].speed = 0;
					macs[i + index].num_lmacs = 0;
					break;
				}

				fifo_cnt += macs[i + index].fifo_cnt;
			}
		}
	}

	/* If more fifos than available were assigned, reduce the number of
	 * fifos until within limit. Start with the lowest priority macs with 4
	 * fifos.
	 */
	prio = 1;
	cnt = 4;
	while (fifo_cnt > get_num_fifos()) {
		for (i = 0; i < get_num_output_macs(); i++) {
			if (macs[i].prio == prio && macs[i].fifo_cnt == cnt) {
				macs[i].fifo_cnt >>= 1;
				fifo_cnt -= macs[i].fifo_cnt;
			}

			if (fifo_cnt <= get_num_fifos())
				break;
		}

		if (prio >= 3) {
			prio = 1;
			cnt >>= 1;
		} else {
			prio++;
		}

		if (cnt == 0)
			break;
	}

	/* Assign left over fifos to dpi */
	if (get_num_fifos() - fifo_cnt > 0) {
		if (get_num_fifos() - fifo_cnt >= 3) {
			macs[1].fifo_cnt += 3;
			fifo_cnt -= 3;
		} else {
			macs[1].fifo_cnt += 1;
			fifo_cnt -= 1;
		}
	}

	return 0;
}

static int get_75xx_fifos_required(int node, struct mac_info *macs)
{
	int		fifo_cnt = 0;
	int		bgx;
	int		index;
	int		qlm;
	enum port_mode	mode;
	int		i;
	int		cnt;
	int		prio;
	u64		data;

	/* The loopback mac gets 1 fifo by default */
	macs[0].fifo_cnt = 1;
	macs[0].speed = 1;
	fifo_cnt += 1;

	/* The dpi mac gets 1 fifo by default */
	macs[1].fifo_cnt = 1;
	macs[1].speed = 50;
	fifo_cnt += 1;

	/* Assign fifos to the active bgx macs */
	bgx = 0;
	for (i = 2; i < 6; i++) {
		index = i - 2;
		qlm = bgx_port_get_qlm(node, bgx, index);
		data = oct_csr_read(GSER_CFG(node, qlm));
		if (data & BIT(2)) {
			macs[i].num_lmacs = 1;

			mode = bgx_port_get_mode(node, bgx, index);
			switch (mode) {
			case PORT_MODE_SGMII:
			case PORT_MODE_RGMII:
				macs[i].fifo_cnt = 1;
				macs[i].prio = 1;
				macs[i].speed = 1;
				break;

			case PORT_MODE_10G_KR:
			case PORT_MODE_XFI:
				macs[i].fifo_cnt = 4;
				macs[i].prio = 2;
				macs[i].speed = 10;
				break;

			default:
				macs[i].fifo_cnt = 0;
				macs[i].prio = 0;
				macs[i].speed = 0;
				macs[i].num_lmacs = 0;
				break;
			}

			fifo_cnt += macs[i].fifo_cnt;
		}
	}

	/* If more fifos than available were assigned, reduce the number of
	 * fifos until within limit. Start with the lowest priority macs with 4
	 * fifos.
	 */
	prio = 1;
	cnt = 4;
	while (fifo_cnt > get_num_fifos()) {
		for (i = 0; i < get_num_output_macs(); i++) {
			if (macs[i].prio == prio && macs[i].fifo_cnt == cnt) {
				macs[i].fifo_cnt >>= 1;
				fifo_cnt -= macs[i].fifo_cnt;
			}

			if (fifo_cnt <= get_num_fifos())
				break;
		}

		if (prio >= 3) {
			prio = 1;
			cnt >>= 1;
		} else {
			prio++;
		}

		if (cnt == 0)
			break;
	}

	/* Assign left over fifos to dpi */
	if (get_num_fifos() - fifo_cnt > 0) {
		if (get_num_fifos() - fifo_cnt >= 3) {
			macs[1].fifo_cnt += 3;
			fifo_cnt -= 3;
		} else {
			macs[1].fifo_cnt += 1;
			fifo_cnt -= 1;
		}
	}

	return 0;
}

static int get_73xx_fifos_required(int node, struct mac_info *macs)
{
	int		fifo_cnt = 0;
	int		bgx;
	int		index;
	int		qlm;
	int		num_lmacs;
	enum port_mode	mode;
	int		i;
	int		cnt;
	int		prio;
	u64		data;

	/* The loopback mac gets 1 fifo by default */
	macs[0].fifo_cnt = 1;
	macs[0].speed = 1;
	fifo_cnt += 1;

	/* The dpi mac gets 1 fifo by default */
	macs[1].fifo_cnt = 1;
	macs[1].speed = 50;
	fifo_cnt += 1;

	/* Assign fifos to the active bgx macs */
	for (i = 2; i < get_num_output_macs(); i += 4) {
		bgx = (i - 2) / 4;
		qlm = bgx_port_get_qlm(node, bgx, 0);
		data = oct_csr_read(GSER_CFG(node, qlm));

		/* Bgx2 can be connected to dlm 5, 6, or both */
		if (bgx == 2) {
			if (!(data & BIT(2))) {
				qlm = bgx_port_get_qlm(node, bgx, 2);
				data = oct_csr_read(GSER_CFG(node, qlm));
			}
		}

		if (data & BIT(2)) {
			data = oct_csr_read(BGX_CMR_TX_LMACS(node, bgx));
			num_lmacs = data & 7;

			for (index = 0; index < num_lmacs; index++) {
				switch (num_lmacs) {
				case 1:
					macs[i + index].num_lmacs = 4;
					break;
				case 2:
					macs[i + index].num_lmacs = 2;
					break;
				case 4:
				default:
					macs[i + index].num_lmacs = 1;
					break;
				}

				mode = bgx_port_get_mode(node, bgx, index);
				switch (mode) {
				case PORT_MODE_SGMII:
				case PORT_MODE_RGMII:
					macs[i + index].fifo_cnt = 1;
					macs[i + index].prio = 1;
					macs[i + index].speed = 1;
					break;

				case PORT_MODE_XAUI:
				case PORT_MODE_RXAUI:
					macs[i + index].fifo_cnt = 4;
					macs[i + index].prio = 2;
					macs[i + index].speed = 20;
					break;

				case PORT_MODE_10G_KR:
				case PORT_MODE_XFI:
					macs[i + index].fifo_cnt = 4;
					macs[i + index].prio = 2;
					macs[i + index].speed = 10;
					break;

				case PORT_MODE_40G_KR4:
				case PORT_MODE_XLAUI:
					macs[i + index].fifo_cnt = 4;
					macs[i + index].prio = 3;
					macs[i + index].speed = 40;
					break;

				default:
					macs[i + index].fifo_cnt = 0;
					macs[i + index].prio = 0;
					macs[i + index].speed = 0;
					break;
				}

				fifo_cnt += macs[i + index].fifo_cnt;
			}
		}
	}

	/* If more fifos than available were assigned, reduce the number of
	 * fifos until within limit. Start with the lowest priority macs with 4
	 * fifos.
	 */
	prio = 1;
	cnt = 4;
	while (fifo_cnt > get_num_fifos()) {
		for (i = 0; i < get_num_output_macs(); i++) {
			if (macs[i].prio == prio && macs[i].fifo_cnt == cnt) {
				macs[i].fifo_cnt >>= 1;
				fifo_cnt -= macs[i].fifo_cnt;
			}

			if (fifo_cnt <= get_num_fifos())
				break;
		}

		if (prio >= 3) {
			prio = 1;
			cnt >>= 1;
		} else {
			prio++;
		}

		if (cnt == 0)
			break;
	}

	/* Assign left over fifos to dpi */
	if (get_num_fifos() - fifo_cnt > 0) {
		if (get_num_fifos() - fifo_cnt >= 3) {
			macs[1].fifo_cnt += 3;
			fifo_cnt -= 3;
		} else {
			macs[1].fifo_cnt += 1;
			fifo_cnt -= 1;
		}
	}

	return 0;
}

static int setup_macs(int node)
{
	struct mac_info		macs[MAX_OUTPUT_MAC];
	struct fifo_grp_info	fifo_grp[MAX_FIFO_GRP];
	int			cnt;
	int			fifo;
	int			grp;
	int			i;
	u64			data;
	int			size;

	memset(macs, 0, sizeof(macs));
	memset(fifo_grp, 0, sizeof(fifo_grp));

	/* Get the number of fifos required by each mac */
	if (OCTEON_IS_MODEL(OCTEON_CN78XX)) {
		get_78xx_fifos_required(node, macs);
	} else if (OCTEON_IS_MODEL(OCTEON_CNF75XX)) {
		get_75xx_fifos_required(node, macs);
	} else if (OCTEON_IS_MODEL(OCTEON_CN73XX)) {
		get_73xx_fifos_required(node, macs);
	} else {
		pr_err("octeon3-pko: Unsupported board type\n");
		return -1;
	}

	/* Assign fifos to each mac. Start with macs requiring 4 fifos */
	fifo = 0;
	for (cnt = 4; cnt > 0; cnt >>= 1) {
		for (i = 0; i < get_num_output_macs(); i++) {
			if (macs[i].fifo_cnt != cnt)
				continue;

			macs[i].fifo = fifo;
			grp = fifo / 4;

			fifo_grp[grp].speed += macs[i].speed;

			if (cnt == 4) {
				/* 10, 0, 0, 0 */
				fifo_grp[grp].size = 4;
			} else if (cnt == 2) {
				/* 5, 0, 5, 0 */
				fifo_grp[grp].size = 3;
			} else if (cnt == 1) {
				if ((fifo & 0x2) && fifo_grp[grp].size == 3) {
					/* 5, 0, 2.5, 2.5 */
					fifo_grp[grp].size = 1;
				} else {
					/* 2.5, 2.5, 2.5, 2.5 */
					fifo_grp[grp].size = 0;
				}
			}

			fifo += cnt;
		}
	}

	/* Configure the fifo groups */
	for (i = 0; i < get_num_fifo_groups(); i++) {
		data = oct_csr_read(PKO_PTGF_CFG(node, i));
		size = data & GENMASK_ULL(2, 0);
		if (size != fifo_grp[i].size)
			data |= BIT(6);
		data &= ~GENMASK_ULL(2, 0);
		data |= fifo_grp[i].size;

		data &= ~GENMASK_ULL(5, 3);
		if (fifo_grp[i].speed >= 40) {
			if (fifo_grp[i].size >= 3) {
				/* 50 Gbps */
				data |= 0x3 << 3;
			} else {
				/* 25 Gbps */
				data |= 0x2 << 3;
			}
		} else if (fifo_grp[i].speed >= 20) {
			/* 25 Gbps */
			data |= 0x2 << 3;
		} else if (fifo_grp[i].speed >= 10) {
			/* 12.5 Gbps */
			data |= 0x1 << 3;
		}
		oct_csr_write(data, PKO_PTGF_CFG(node, i));
		data &= ~BIT(6);
		oct_csr_write(data, PKO_PTGF_CFG(node, i));
	}

	/* Configure the macs with their assigned fifo */
	for (i = 0; i < get_num_output_macs(); i++) {
		data = oct_csr_read(PKO_MAC_CFG(node, i));
		data &= ~GENMASK_ULL(4, 0);
		if (!macs[i].fifo_cnt)
			data |= 0x1f;
		else
			data |= macs[i].fifo;
		oct_csr_write(data, PKO_MAC_CFG(node, i));
	}

	/* Setup mci0/mci1/skid credits */
	for (i = 0; i < get_num_output_macs(); i++) {
		int	fifo_credit;
		int	mac_credit;
		int	skid_credit;

		if (!macs[i].fifo_cnt)
			continue;

		if (i == 0) {
			/* Loopback */
			mac_credit = 4 * 1024;
			skid_credit = 0;
		} else if (i == 1) {
			/* Dpi */
			mac_credit = 2 * 1024;
			skid_credit = 0;
		} else if (OCTEON_IS_MODEL(OCTEON_CN78XX) && ((i == 2 || i == 3))) {
			/* ILK */
			mac_credit = 4 * 1024;
			skid_credit = 0;
		} else if (OCTEON_IS_MODEL(OCTEON_CNF75XX) && ((i >= 6 && i <= 9))) {
			/* Srio */
			mac_credit = 1024 / 2;
			skid_credit = 0;
		} else {
			/* Bgx */
			mac_credit = macs[i].num_lmacs * 8 * 1024;
			skid_credit = macs[i].num_lmacs * 256;
		}

		if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X)) {
			fifo_credit = macs[i].fifo_cnt * FIFO_SIZE;
			data = (fifo_credit + mac_credit) / 16;
			oct_csr_write(data, PKO_MCI0_MAX_CRED(node, i));
		}

		data = mac_credit / 16;
		oct_csr_write(data, PKO_MCI1_MAX_CRED(node, i));

		data = oct_csr_read(PKO_MAC_CFG(node, i));
		data &= ~GENMASK_ULL(6, 5);
		data |= ((skid_credit / 256) >> 1) << 5;
		oct_csr_write(data, PKO_MAC_CFG(node, i));
	}

	return 0;
}

static int hw_init_global(int node, int aura)
{
	u64	data;
	int	timeout;

	data = oct_csr_read(PKO_ENABLE(node));
	if (data & BIT(0)) {
		pr_info("octeon3-pko: Pko already enabled on node %d\n", node);
		return 0;
	}

	/* Enable color awareness */
	data = oct_csr_read(PKO_SHAPER_CFG(node));
	data |= BIT(1);
	oct_csr_write(data, PKO_SHAPER_CFG(node));

	/* Clear flush command */
	oct_csr_write(0, PKO_DPFI_FLUSH(node));

	/* Set the aura number */
	data = (node << 10) | aura;
	oct_csr_write(data, PKO_DPFI_FPA_AURA(node));

	data = BIT(0);
	oct_csr_write(data, PKO_DPFI_ENA(node));

	/* Wait until all pointers have been returned */
	timeout = 100000;
	do {
		data = oct_csr_read(PKO_STATUS(node));
		if (data & BIT(63))
			break;
		udelay(1);
		timeout--;
	} while (timeout);
	if (!timeout) {
		pr_err("octeon3-pko: Pko dfpi failed on node %d\n", node);
		return -1;
	}

	/* Set max outstanding requests in IOBP for any FIFO.*/
	data = oct_csr_read(PKO_PTF_IOBP_CFG(node));
	data &= ~GENMASK_ULL(6, 0);
	if (OCTEON_IS_MODEL(OCTEON_CN78XX))
		data |= 0x10;
	else
		data |= 3;
	oct_csr_write(data, PKO_PTF_IOBP_CFG(node));

	/* Set minimum packet size per Ethernet standard */
	data = 0x3c << 3;
	oct_csr_write(data, PKO_PDM_CFG(node));

	/* Initialize macs and fifos */
	setup_macs(node);

	/* Enable pko */
	data = BIT(0);
	oct_csr_write(data, PKO_ENABLE(node));

	/* Verify pko is ready */
	data = oct_csr_read(PKO_STATUS(node));
	if (!(data & BIT(63))) {
		pr_err("octeon3_pko: pko is not ready\n");
		return -1;
	}

	return 0;
}

static int hw_exit_global(int node)
{
	u64	data;
	int	timeout;
	int	i;

	/* Wait until there are no in-flight packets */
	for (i = 0; i < get_num_fifos(); i++) {
		data = oct_csr_read(PKO_PTF_STATUS(node, i));
		if ((data & GENMASK_ULL(4, 0)) == 0x1f)
			continue;

		timeout = 10000;
		do {
			if (!(data & GENMASK_ULL(11, 5)))
				break;
			udelay(1);
			timeout--;
			data = oct_csr_read(PKO_PTF_STATUS(node, i));
		} while (timeout);
		if (!timeout) {
			pr_err("octeon3-pko: Timeout in-flight fifo\n");
			return -1;
		}
	}

	/* Disable pko */
	oct_csr_write(0, PKO_ENABLE(node));

	/* Reset all port queues to the virtual mac */
	for (i = 0; i < get_num_port_queues(); i++) {
		data = get_num_output_macs() << 16;
		oct_csr_write(data, PKO_L1_SQ_TOPOLOGY(node, i));

		data = get_num_output_macs() << 13;
		oct_csr_write(data, PKO_L1_SQ_SHAPE(node, i));

		data = (u64)get_num_output_macs() << 48;
		oct_csr_write(data, PKO_L1_SQ_LINK(node, i));
	}

	/* Reset all output macs */
	for (i = 0; i < get_num_output_macs(); i++) {
		data = 0x1f;
		oct_csr_write(data, PKO_MAC_CFG(node, i));
	}

	/* Reset all fifo groups */
	for (i = 0; i < get_num_fifo_groups(); i++) {
		data = oct_csr_read(PKO_PTGF_CFG(node, i));
		/* Simulator asserts if an unused group is reset */
		if (data == 0)
			continue;
		data = BIT(6);
		oct_csr_write(data, PKO_PTGF_CFG(node, i));
	}

	/* Return cache pointers to fpa */
	data = BIT(0);
	oct_csr_write(data, PKO_DPFI_FLUSH(node));
	timeout = 10000;
	do {
		data = oct_csr_read(PKO_DPFI_STATUS(node));
		if (data & BIT(0))
			break;
		udelay(1);
		timeout--;
	} while (timeout);
	if (!timeout) {
		pr_err("octeon3-pko: Timeout flushing cache\n");
		return -1;
	}
	oct_csr_write(0, PKO_DPFI_ENA(node));
	oct_csr_write(0, PKO_DPFI_FLUSH(node));

	return 0;
}

static int virtual_mac_config(int node)
{
	int			vmac;
	int			pq;
	int			dq[8];
	int			num_dq;
	int			parent_q;
	enum queue_level	level;
	int			queue;
	int			i;
	int			rc;

	/* The virtual mac is after the last output mac. Note: for the 73xx it
	 * might be 2 after the last output mac (15).
	 */
	vmac = get_num_output_macs();

	/* Allocate a port queue */
	rc = allocate_queues(node, PQ, 1, &pq);
	if (rc < 0) {
		pr_err("octeon3-pko: Failed to allocate port queue\n");
		return rc;
	}

	/* Connect the port queue to the output mac */
	port_queue_init(node, pq, vmac);

	parent_q = pq;
	for (level = L2_SQ; level <= max_sq_level(); level++) {
		rc = allocate_queues(node, level, 1, &queue);
		if (rc < 0) {
			pr_err("octeon3-pko: Failed to allocate queue\n");
			return rc;
		}

		switch (level) {
		case L2_SQ:
			scheduler_queue_l2_init(node, queue, parent_q);
			break;
		case L3_SQ:
			scheduler_queue_l3_init(node, queue, parent_q);
			break;
		case L4_SQ:
			scheduler_queue_l4_init(node, queue, parent_q);
			break;
		case L5_SQ:
			scheduler_queue_l5_init(node, queue, parent_q);
			break;
		default:
			break;
		}

		parent_q = queue;
	}

	if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_0))
		num_dq = 8;
	else
		num_dq = 1;

	rc = allocate_queues(node, DQ, num_dq, dq);
	if (rc < 0) {
		pr_err("octeon3-pko: Failed to allocate description queues\n");
		return rc;
	}

	/* By convention the dq must be zero */
	if (dq[0] != 0) {
		pr_err("octeon3-pko: Failed to reserve description queues\n");
		return -1;
	}
	descriptor_queue_init(node, dq, parent_q, num_dq);

	/* Open the dqs */
	for (i = 0; i < num_dq; i++)
		open_dq(node, dq[i]);

	return 0;
}

static int drain_dq(int node, int dq)
{
	u64	data;
	int	timeout;
	s64	rc;

	data = BIT(2) | BIT(1);
	oct_csr_write(data, PKO_DQ_SW_XOFF(node, dq));

	usleep_range(1000, 2000);

	data = 0;
	oct_csr_write(data, PKO_DQ_SW_XOFF(node, dq));

	/* Wait for the dq to drain */
	timeout = 10000;
	do {
		rc = query_dq(node, dq);
		if (!rc)
			break;
		else if (rc < 0)
			return rc;
		udelay(1);
		timeout--;
	} while (timeout);
	if (!timeout) {
		pr_err("octeon3-pko: Timeout waiting for dq to drain\n");
		return -1;
	}

	/* Close the queue anf free internal buffers */
	close_dq(node, dq);

	return 0;
}

int octeon3_pko_exit_global(int node)
{
	int	dq[8];
	int	num_dq;
	int	i;

	if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_0))
		num_dq = 8;
	else
		num_dq = 1;

	/* Shutdown the virtual/null interface */
	for (i = 0; i < ARRAY_SIZE(dq); i++)
		dq[i] = i;
	octeon3_pko_interface_uninit(node, dq, num_dq);

	/* Shutdown pko */
	hw_exit_global(node);

	return 0;
}
EXPORT_SYMBOL(octeon3_pko_exit_global);

int octeon3_pko_init_global(int node, int aura)
{
	int	rc;

	rc = hw_init_global(node, aura);
	if (rc)
		return rc;

	/* Channel credit level at level 2 */
	oct_csr_write(0, PKO_CHANNEL_LEVEL(node));

	/* Configure the null mac */
	rc = virtual_mac_config(node);
	if (rc)
		return rc;

	return 0;
}
EXPORT_SYMBOL(octeon3_pko_init_global);

int octeon3_pko_set_mac_options(int			node,
				int			interface,
				int			index,
				enum octeon3_mac_type	mac_type,
				bool			fcs_en,
				bool			pad_en,
				int			fcs_sop_off)
{
	int	mac;
	u64	data;
	int	fifo_num;

	mac = get_output_mac(interface, index, mac_type);

	data = oct_csr_read(PKO_MAC_CFG(node, mac));
	fifo_num = data & GENMASK_ULL(4, 0);
	if (fifo_num == 0x1f) {
		pr_err("octeon3_pko: mac not configured %d:%d:%d\n", node, interface, index);
		return -ENODEV;
	}

	/* Some silicon requires fifo_num=0x1f to change padding, fcs */
	data &= ~GENMASK_ULL(4, 0);
	data |= 0x1f;

	data &= ~(BIT(16) | BIT(15) | GENMASK_ULL(14, 7));
	if (pad_en)
		data |= BIT(16);
	if (fcs_en)
		data |= BIT(15);
	if (fcs_sop_off)
		data |= fcs_sop_off << 7;

	oct_csr_write(data, PKO_MAC_CFG(node, mac));

	data &= ~GENMASK_ULL(4, 0);
	data |= fifo_num;
	oct_csr_write(data, PKO_MAC_CFG(node, mac));

	return 0;
}
EXPORT_SYMBOL(octeon3_pko_set_mac_options);

int octeon3_pko_get_fifo_size(int			node,
			      int			interface,
			      int			index,
			      enum octeon3_mac_type	mac_type)
{
	int	mac;
	u64	data;
	int	fifo_grp;
	int	fifo_off;
	int	size;

	/* Set fifo size to 2.4 KB */
	size = FIFO_SIZE;

	mac = get_output_mac(interface, index, mac_type);

	data = oct_csr_read(PKO_MAC_CFG(node, mac));
	if ((data & GENMASK_ULL(4, 0)) == 0x1f) {
		pr_err("octeon3_pko: mac not configured %d:%d:%d\n", node, interface, index);
		return -ENODEV;
	}
	fifo_grp = (data & GENMASK_ULL(4, 0)) >> 2;
	fifo_off = data & GENMASK_ULL(1, 0);

	data = oct_csr_read(PKO_PTGF_CFG(node, fifo_grp));
	data &= GENMASK_ULL(2, 0);
	switch (data) {
	case 0:
		/* 2.5l, 2.5k, 2.5k, 2.5k */
		break;
	case 1:
		/* 5.0k, 0.0k, 2.5k, 2.5k */
		if (fifo_off == 0)
			size *= 2;
		if (fifo_off == 1)
			size = 0;
		break;
	case 2:
		/* 2.5k, 2.5k, 5.0k, 0.0k */
		if (fifo_off == 2)
			size *= 2;
		if (fifo_off == 3)
			size = 0;
		break;
	case 3:
		/* 5k, 0, 5k, 0 */
		if ((fifo_off & 1) != 0)
			size = 0;
		size *= 2;
		break;
	case 4:
		/* 10k, 0, 0, 0 */
		if (fifo_off != 0)
			size = 0;
		size *= 4;
		break;
	default:
		size = -1;
	}

	return size;
}
EXPORT_SYMBOL(octeon3_pko_get_fifo_size);

int octeon3_pko_activate_dq(int node, int dq, int cnt)
{
	int	i;
	int	rc = 0;
	u64	data;

	for (i = 0; i < cnt; i++) {
		rc = open_dq(node, dq + i);
		if (rc)
			break;

		data = oct_csr_read(PKO_PDM_DQ_MINPAD(node, dq + i));
		data &= ~BIT(0);
		oct_csr_write(data, PKO_PDM_DQ_MINPAD(node, dq + i));
	}

	return rc;
}
EXPORT_SYMBOL(octeon3_pko_activate_dq);

int octeon3_pko_interface_init(int			node,
			       int			interface,
			       int			index,
			       enum octeon3_mac_type	mac_type,
			       int			ipd_port)
{
	int			mac;
	int			pq;
	int			parent_q;
	int			queue;
	enum queue_level	level;
	int			rc;

	mac = get_output_mac(interface, index, mac_type);

	/* Allocate a port queue for this interface */
	rc = allocate_queues(node, PQ, 1, &pq);
	if (rc < 0) {
		pr_err("octeon3-pko: Failed to allocate port queue\n");
		return rc;
	}

	/* Connect the port queue to the output mac */
	port_queue_init(node, pq, mac);

	/* Link scheduler queues to the port queue */
	parent_q = pq;
	for (level = L2_SQ; level <= max_sq_level(); level++) {
		rc = allocate_queues(node, level, 1, &queue);
		if (rc < 0) {
			pr_err("octeon3-pko: Failed to allocate queue\n");
			return rc;
		}

		switch (level) {
		case L2_SQ:
			scheduler_queue_l2_init(node, queue, parent_q);
			map_channel(node, pq, queue, ipd_port);
			break;
		case L3_SQ:
			scheduler_queue_l3_init(node, queue, parent_q);
			break;
		case L4_SQ:
			scheduler_queue_l4_init(node, queue, parent_q);
			break;
		case L5_SQ:
			scheduler_queue_l5_init(node, queue, parent_q);
			break;
		default:
			break;
		}

		parent_q = queue;
	}

	/* Link the descriptor queue */
	rc = allocate_queues(node, DQ, 1, &queue);
	if (rc < 0) {
		pr_err("octeon3-pko: Failed to allocate descriptor queue\n");
		return rc;
	}
	descriptor_queue_init(node, &queue, parent_q, 1);

	return queue;
}
EXPORT_SYMBOL(octeon3_pko_interface_init);

int octeon3_pko_interface_uninit(int		node,
				 const int	*dq,
				 int		num_dq)
{
	enum queue_level	level;
	int			queue;
	int			parent_q;
	u64			data;
	u64			addr;
	int			i;
	int			rc;

	/* Drain all dqs */
	for (i = 0; i < num_dq; i++) {
		rc = drain_dq(node, dq[i]);
		if (rc)
			return rc;

		/* Free the dq */
		data = oct_csr_read(PKO_DQ_TOPOLOGY(node, dq[i]));
		parent_q = (data & GENMASK_ULL(25, 16)) >> 16;
		free_queues(node, DQ, 1, &dq[i]);

		/* Free all the scheduler queues */
		queue = parent_q;
		for (level = max_sq_level(); (signed int)level >= PQ; level--) {
			switch (level) {
			case L5_SQ:
				addr = PKO_L5_SQ_TOPOLOGY(node, queue);
				data = oct_csr_read(addr);
				parent_q = (data & GENMASK_ULL(25, 16)) >> 16;
				break;

			case L4_SQ:
				addr = PKO_L4_SQ_TOPOLOGY(node, queue);
				data = oct_csr_read(addr);
				parent_q = (data & GENMASK_ULL(24, 16)) >> 16;
				break;

			case L3_SQ:
				addr = PKO_L3_SQ_TOPOLOGY(node, queue);
				data = oct_csr_read(addr);
				parent_q = (data & GENMASK_ULL(24, 16)) >> 16;
				break;

			case L2_SQ:
				addr = PKO_L2_SQ_TOPOLOGY(node, queue);
				data = oct_csr_read(addr);
				parent_q = (data & GENMASK_ULL(20, 16)) >> 16;
				break;

			case PQ:
				break;

			default:
				pr_err("octeon3-pko: Invalid level=%d\n",
				       level);
				return -1;
			}

			free_queues(node, level, 1, &queue);
			queue = parent_q;
		}
	}

	return 0;
}
EXPORT_SYMBOL(octeon3_pko_interface_uninit);
