// SPDX-License-Identifier: GPL-2.0
/* Octeon III Packet Input Unit (PKI)
 *
 * Copyright (C) 2018 Cavium, Inc.
 */

#include <linux/firmware.h>

#include "octeon3.h"

#define PKI_CLUSTER_FIRMWARE		"cavium/pki-cluster.bin"
#define VERSION_LEN			8

#define MAX_CLUSTERS			4
#define MAX_BANKS			2
#define MAX_BANK_ENTRIES		192
#define PKI_NUM_QPG_ENTRY		2048
#define PKI_NUM_STYLE			256
#define PKI_NUM_FINAL_STYLE		64
#define MAX_PKNDS			64

/* Registers are accessed via xkphys */
#define PKI_BASE			0x1180044000000ull
#define PKI_ADDR(node)			(SET_XKPHYS + NODE_OFFSET(node) +      \
					 PKI_BASE)

#define PKI_SFT_RST(n)			(PKI_ADDR(n)		     + 0x000010)
#define PKI_BUF_CTL(n)			(PKI_ADDR(n)		     + 0x000100)
#define PKI_STAT_CTL(n)			(PKI_ADDR(n)		     + 0x000110)
#define PKI_ICG_CFG(n)			(PKI_ADDR(n)		     + 0x00a000)

#define CLUSTER_OFFSET(c)		((c) << 16)
#define CL_ADDR(n, c)			(PKI_ADDR(n) + CLUSTER_OFFSET(c))
#define PKI_CL_ECC_CTL(n, c)		(CL_ADDR(n, c)		     + 0x00c020)

#define PKI_STYLE_BUF(n, s)		(PKI_ADDR(n) + ((s) << 3)    + 0x024000)

#define PKI_LTYPE_MAP(n, l)		(PKI_ADDR(n) + ((l) << 3)    + 0x005000)
#define PKI_IMEM(n, i)			(PKI_ADDR(n) + ((i) << 3)    + 0x100000)

#define PKI_CL_PKIND_CFG(n, c, p)	(CL_ADDR(n, c) + ((p) << 8)  + 0x300040)
#define PKI_CL_PKIND_STYLE(n, c, p)	(CL_ADDR(n, c) + ((p) << 8)  + 0x300048)
#define PKI_CL_PKIND_SKIP(n, c, p)	(CL_ADDR(n, c) + ((p) << 8)  + 0x300050)
#define PKI_CL_PKIND_L2_CUSTOM(n, c, p)	(CL_ADDR(n, c) + ((p) << 8)  + 0x300058)
#define PKI_CL_PKIND_LG_CUSTOM(n, c, p)	(CL_ADDR(n, c) + ((p) << 8)  + 0x300060)

#define STYLE_OFFSET(s)			((s) << 3)
#define STYLE_ADDR(n, c, s)		(PKI_ADDR(n) + CLUSTER_OFFSET(c) +     \
					 STYLE_OFFSET(s))
#define PKI_CL_STYLE_CFG(n, c, s)	(STYLE_ADDR(n, c, s)	     + 0x500000)
#define PKI_CL_STYLE_CFG2(n, c, s)	(STYLE_ADDR(n, c, s)	     + 0x500800)
#define PKI_CLX_STYLEX_ALG(n, c, s)	(STYLE_ADDR(n, c, s)	     + 0x501000)

#define PCAM_OFFSET(bank)		((bank) << 12)
#define PCAM_ENTRY_OFFSET(entry)	((entry) << 3)
#define PCAM_ADDR(n, c, b, e)		(PKI_ADDR(n) + CLUSTER_OFFSET(c) +     \
					 PCAM_OFFSET(b) + PCAM_ENTRY_OFFSET(e))
#define PKI_CL_PCAM_TERM(n, c, b, e)	(PCAM_ADDR(n, c, b, e)	     + 0x700000)
#define PKI_CL_PCAM_MATCH(n, c, b, e)	(PCAM_ADDR(n, c, b, e)	     + 0x704000)
#define PKI_CL_PCAM_ACTION(n, c, b, e)	(PCAM_ADDR(n, c, b, e)	     + 0x708000)

#define PKI_QPG_TBLX(n, i)		(PKI_ADDR(n) + ((i) << 3)    + 0x800000)
#define PKI_AURAX_CFG(n, a)		(PKI_ADDR(n) + ((a) << 3)    + 0x900000)
#define PKI_STATX_STAT0(n, p)		(PKI_ADDR(n) + ((p) << 8)    + 0xe00038)
#define PKI_STATX_STAT1(n, p)		(PKI_ADDR(n) + ((p) << 8)    + 0xe00040)
#define PKI_STATX_STAT3(n, p)		(PKI_ADDR(n) + ((p) << 8)    + 0xe00050)

enum pcam_term {
	NONE,
	L2_CUSTOM = 0x2,
	HIGIGD = 0x4,
	HIGIG = 0x5,
	SMACH = 0x8,
	SMACL = 0x9,
	DMACH = 0xa,
	DMACL = 0xb,
	GLORT = 0x12,
	DSA = 0x13,
	ETHTYPE0 = 0x18,
	ETHTYPE1 = 0x19,
	ETHTYPE2 = 0x1a,
	ETHTYPE3 = 0x1b,
	MPLS0 = 0x1e,
	L3_SIPHH = 0x1f,
	L3_SIPMH = 0x20,
	L3_SIPML = 0x21,
	L3_SIPLL = 0x22,
	L3_FLAGS = 0x23,
	L3_DIPHH = 0x24,
	L3_DIPMH = 0x25,
	L3_DIPML = 0x26,
	L3_DIPLL = 0x27,
	LD_VNI = 0x28,
	IL3_FLAGS = 0x2b,
	LF_SPI = 0x2e,
	L4_SPORT = 0x2f,
	L4_PORT = 0x30,
	LG_CUSTOM = 0x39
};

enum pki_ltype {
	LTYPE_NONE,
	LTYPE_ENET,
	LTYPE_VLAN,
	LTYPE_SNAP_PAYLD = 0x05,
	LTYPE_ARP = 0x06,
	LTYPE_RARP = 0x07,
	LTYPE_IP4 = 0x08,
	LTYPE_IP4_OPT = 0x09,
	LTYPE_IP6 = 0x0a,
	LTYPE_IP6_OPT = 0x0b,
	LTYPE_IPSEC_ESP = 0x0c,
	LTYPE_IPFRAG = 0x0d,
	LTYPE_IPCOMP = 0x0e,
	LTYPE_TCP = 0x10,
	LTYPE_UDP = 0x11,
	LTYPE_SCTP = 0x12,
	LTYPE_UDP_VXLAN = 0x13,
	LTYPE_GRE = 0x14,
	LTYPE_NVGRE = 0x15,
	LTYPE_GTP = 0x16,
	LTYPE_UDP_GENEVE = 0x17,
	LTYPE_SW28 = 0x1c,
	LTYPE_SW29 = 0x1d,
	LTYPE_SW30 = 0x1e,
	LTYPE_SW31 = 0x1f
};

enum pki_beltype {
	BELTYPE_NONE,
	BELTYPE_MISC,
	BELTYPE_IP4,
	BELTYPE_IP6,
	BELTYPE_TCP,
	BELTYPE_UDP,
	BELTYPE_SCTP,
	BELTYPE_SNAP
};

struct ltype_beltype {
	enum pki_ltype ltype;
	enum pki_beltype beltype;
};

/* struct pcam_term_info - Describes a term to configure in the PCAM.
 * @term: Identifies the term to configure.
 * @term_mask: Specifies don't cares in the term.
 * @style: Style to compare.
 * @style_mask: Specifies don't cares in the style.
 * @data: Data to compare.
 * @data_mask: Specifies don't cares in the data.
 */
struct pcam_term_info {
	u8 term;
	u8 term_mask;
	u8 style;
	u8 style_mask;
	u32 data;
	u32 data_mask;
};

/* struct fw_hdr - Describes the firmware.
 * @version: Firmware version.
 * @size: Size of the data in bytes.
 * @data: Actual firmware data.
 */
struct fw_hdr {
	char version[VERSION_LEN];
	u64 size;
	u64 data[];
};

static struct ltype_beltype dflt_ltype_config[] = {
	{ LTYPE_NONE, BELTYPE_NONE },
	{ LTYPE_ENET, BELTYPE_MISC },
	{ LTYPE_VLAN, BELTYPE_MISC },
	{ LTYPE_SNAP_PAYLD, BELTYPE_MISC },
	{ LTYPE_ARP, BELTYPE_MISC },
	{ LTYPE_RARP, BELTYPE_MISC },
	{ LTYPE_IP4, BELTYPE_IP4 },
	{ LTYPE_IP4_OPT, BELTYPE_IP4 },
	{ LTYPE_IP6, BELTYPE_IP6 },
	{ LTYPE_IP6_OPT, BELTYPE_IP6 },
	{ LTYPE_IPSEC_ESP, BELTYPE_MISC },
	{ LTYPE_IPFRAG, BELTYPE_MISC },
	{ LTYPE_IPCOMP, BELTYPE_MISC },
	{ LTYPE_TCP, BELTYPE_TCP },
	{ LTYPE_UDP, BELTYPE_UDP },
	{ LTYPE_SCTP, BELTYPE_SCTP },
	{ LTYPE_UDP_VXLAN, BELTYPE_UDP },
	{ LTYPE_GRE, BELTYPE_MISC },
	{ LTYPE_NVGRE, BELTYPE_MISC },
	{ LTYPE_GTP, BELTYPE_MISC },
	{ LTYPE_UDP_GENEVE, BELTYPE_UDP },
	{ LTYPE_SW28, BELTYPE_MISC },
	{ LTYPE_SW29, BELTYPE_MISC },
	{ LTYPE_SW30, BELTYPE_MISC },
	{ LTYPE_SW31, BELTYPE_MISC }
};

static int get_num_clusters(void)
{
	if (OCTEON_IS_MODEL(OCTEON_CN73XX) || OCTEON_IS_MODEL(OCTEON_CNF75XX))
		return 2;
	return 4;
}

static int octeon3_pki_pcam_alloc_entry(int node, int entry, int bank)
{
	struct global_resource_tag tag;
	int i, rc, num_clusters;
	char buf[16];

	/* Allocate a PCAM entry for cluster0. */
	strncpy((char *)&tag.lo, "cvm_pcam", 8);
	snprintf(buf, 16, "_%d%d%d....", node, 0, bank);
	memcpy(&tag.hi, buf, 8);

	res_mgr_create_resource(tag, MAX_BANK_ENTRIES);
	rc = res_mgr_alloc(tag, entry, false);
	if (rc < 0)
		return rc;

	entry = rc;

	/* Allocate entries for all clusters. */
	num_clusters = get_num_clusters();
	for (i = 1; i < num_clusters; i++) {
		strncpy((char *)&tag.lo, "cvm_pcam", 8);
		snprintf(buf, 16, "_%d%d%d....", node, i, bank);
		memcpy(&tag.hi, buf, 8);

		res_mgr_create_resource(tag, MAX_BANK_ENTRIES);
		rc = res_mgr_alloc(tag, entry, false);
		if (rc < 0) {
			int	j;

			pr_err("%s: Failed to allocate PCAM entry\n", __FILE__);
			/* Undo whatever we've did */
			for (j = 0; i < i; j++) {
				strncpy((char *)&tag.lo, "cvm_pcam", 8);
				snprintf(buf, 16, "_%d%d%d....", node, j, bank);
				memcpy(&tag.hi, buf, 8);
				res_mgr_free(tag, entry);
			}

			return -1;
		}
	}

	return entry;
}

static int octeon3_pki_pcam_write_entry(int node,
					struct pcam_term_info *term_info)
{
	int bank, entry, i, num_clusters;
	u64 action, match, term;

	/* Bit 0 of the PCAM term determines the bank to use. */
	bank = term_info->term & 1;

	/* Allocate a PCAM entry. */
	entry = octeon3_pki_pcam_alloc_entry(node, -1, bank);
	if (entry < 0)
		return entry;

	term = 1ull << 63;
	term |= (u64)(term_info->term & term_info->term_mask) << 40;
	term |= (~term_info->term & term_info->term_mask) << 8;
	term |= (u64)(term_info->style & term_info->style_mask) << 32;
	term |= ~term_info->style & term_info->style_mask;

	match = (u64)(term_info->data & term_info->data_mask) << 32;
	match |= ~term_info->data & term_info->data_mask;

	action = 0;
	if (term_info->term >= ETHTYPE0 && term_info->term <= ETHTYPE3) {
		action |= 2 << 8;
		action |= 4;
	}

	/* Must write the term to all clusters. */
	num_clusters = get_num_clusters();
	for (i = 0; i < num_clusters; i++) {
		oct_csr_write(0, PKI_CL_PCAM_TERM(node, i, bank, entry));
		oct_csr_write(match, PKI_CL_PCAM_MATCH(node, i, bank, entry));
		oct_csr_write(action, PKI_CL_PCAM_ACTION(node, i, bank, entry));
		oct_csr_write(term, PKI_CL_PCAM_TERM(node, i, bank, entry));
	}

	return 0;
}

static int octeon3_pki_alloc_qpg_entry(int node)
{
	struct global_resource_tag tag;
	char buf[16];
	int entry;

	/* Allocate a Qpg entry. */
	strncpy((char *)&tag.lo, "cvm_qpge", 8);
	snprintf(buf, 16, "t_%d.....", node);
	memcpy(&tag.hi, buf, 8);

	res_mgr_create_resource(tag, PKI_NUM_QPG_ENTRY);
	entry = res_mgr_alloc(tag, -1, false);
	if (entry < 0)
		pr_err("%s: Failed to allocate qpg entry", __FILE__);

	return entry;
}

static int octeon3_pki_alloc_style(int node)
{
	struct global_resource_tag tag;
	char buf[16];
	int entry;

	/* Allocate a style entry. */
	strncpy((char *)&tag.lo, "cvm_styl", 8);
	snprintf(buf, 16, "e_%d.....", node);
	memcpy(&tag.hi, buf, 8);

	res_mgr_create_resource(tag, PKI_NUM_STYLE);
	entry = res_mgr_alloc(tag, -1, false);
	if (entry < 0)
		pr_err("%s: Failed to allocate style", __FILE__);

	return entry;
}

int octeon3_pki_set_ptp_skip(int node, int pknd, int skip)
{
	int i, num_clusters;
	u64 data;

	num_clusters = get_num_clusters();
	for (i = 0; i < num_clusters; i++) {
		data = oct_csr_read(PKI_CL_PKIND_SKIP(node, i, pknd));
		data &= ~(GENMASK_ULL(15, 8) | GENMASK_ULL(7, 0));
		data |= (skip << 8) | skip;
		oct_csr_write(data, PKI_CL_PKIND_SKIP(node, i, pknd));

		data = oct_csr_read(PKI_CL_PKIND_L2_CUSTOM(node, i, pknd));
		data &= ~GENMASK_ULL(7, 0);
		data |= skip;
		oct_csr_write(data, PKI_CL_PKIND_L2_CUSTOM(node, i, pknd));
	}

	return 0;
}
EXPORT_SYMBOL(octeon3_pki_set_ptp_skip);

/* octeon3_pki_get_stats - Get the statistics for a given pknd (port).
 * @node: Node to get statistics for.
 * @pknd: Pknd to get statistis for.
 * @packets: Updated with the number of packets received.
 * @octets: Updated with the number of octets received.
 * @dropped: Updated with the number of dropped packets.
 *
 * Returns 0 if successful.
 * Returns <0 for error codes.
 */
int octeon3_pki_get_stats(int node, int pknd, u64 *packets, u64 *octets,
			  u64 *dropped)
{
	/* PKI-20775, must read until not all ones. */
	do {
		*packets = oct_csr_read(PKI_STATX_STAT0(node, pknd));
	} while (*packets == 0xffffffffffffffffull);

	do {
		*octets = oct_csr_read(PKI_STATX_STAT1(node, pknd));
	} while (*octets == 0xffffffffffffffffull);

	do {
		*dropped = oct_csr_read(PKI_STATX_STAT3(node, pknd));
	} while (*dropped == 0xffffffffffffffffull);

	return 0;
}
EXPORT_SYMBOL(octeon3_pki_get_stats);

/* octeon3_pki_port_init - Initialize a port.
 * @node: Node port is using.
 * @aura: Aura to use for packet buffers.
 * @grp: SSO group packets will be queued up for.
 * @skip: Extra bytes to skip before packet data.
 * @mb_size: Size of packet buffers.
 * @pknd: Port kind assigned to the port.
 * @num_rx_cxt: Number of SSO groups used by the port.
 *
 * Returns 0 if successful.
 * Returns <0 for error codes.
 */
int octeon3_pki_port_init(int node, int aura, int grp, int skip, int mb_size,
			  int pknd, int num_rx_cxt)
{
	int num_clusters, qpg_entry, style;
	u64 data, i;

	/* Allocate and configure a Qpg table entry for the port's group. */
	i = 0;
	while ((num_rx_cxt & (1 << i)) == 0)
		i++;
	qpg_entry = octeon3_pki_alloc_qpg_entry(node);
	data = oct_csr_read(PKI_QPG_TBLX(node, qpg_entry));
	data &= ~(GENMASK_ULL(59, 48) | GENMASK_ULL(47, 45) |
		  GENMASK_ULL(41, 32) | GENMASK_ULL(31, 29) |
		  GENMASK_ULL(25, 16) | GENMASK_ULL(9, 0));
	data |= i << 45;
	data |= ((u64)((node << 8) | grp) << 32);
	data |= i << 29;
	data |= (((node << 8) | grp) << 16);
	data |= aura;
	oct_csr_write(data, PKI_QPG_TBLX(node, qpg_entry));

	/* Allocate a style for the port */
	style = octeon3_pki_alloc_style(node);

	/* Map the Qpg table entry to the style. */
	num_clusters = get_num_clusters();
	for (i = 0; i < num_clusters; i++) {
		data = BIT(29) | BIT(22) | qpg_entry;
		oct_csr_write(data, PKI_CL_STYLE_CFG(node, i, style));

		/* Specify the tag generation rules and checksum to use. */
		oct_csr_write(0xfff49f, PKI_CL_STYLE_CFG2(node, i, style));

		data = BIT(31);
		oct_csr_write(data, PKI_CLX_STYLEX_ALG(node, i, style));
	}

	/* Set the style's buffer size and skips:
	 *	Every buffer has 128 bytes reserved for Linux.
	 *	The first buffer must also skip the WQE (40 bytes).
	 *	SRIO also requires skipping its header (skip).
	 */
	data = 1ull << 28;
	data |= ((128 + 40 + skip) / 8) << 22;
	data |= (128 / 8) << 16;
	data |= (mb_size & ~0xf) / 8;
	oct_csr_write(data, PKI_STYLE_BUF(node, style));

	/* Assign the initial style to the port via the pknd. */
	for (i = 0; i < num_clusters; i++) {
		data = oct_csr_read(PKI_CL_PKIND_STYLE(node, i, pknd));
		data &= ~GENMASK_ULL(7, 0);
		data |= style;
		oct_csr_write(data, PKI_CL_PKIND_STYLE(node, i, pknd));
	}

	/* Enable red. */
	data = BIT(18);
	oct_csr_write(data, PKI_AURAX_CFG(node, aura));

	/* Clear statistic counters. */
	oct_csr_write(0, PKI_STATX_STAT0(node, pknd));
	oct_csr_write(0, PKI_STATX_STAT1(node, pknd));
	oct_csr_write(0, PKI_STATX_STAT3(node, pknd));

	return 0;
}
EXPORT_SYMBOL(octeon3_pki_port_init);

/* octeon3_pki_port_shutdown - Release all the resources used by a port.
 * @node: Node the port is on.
 * @pknd: Pknd assigned to the port.
 *
 * Returns 0 if successful.
 * Returns <0 for error codes.
 */
int octeon3_pki_port_shutdown(int node, int pknd)
{
	/* Nothing at the moment. */
	return 0;
}
EXPORT_SYMBOL(octeon3_pki_port_shutdown);

/* octeon3_pki_cluster_init - Loads cluster firmware into the PKI clusters.
 * @node: Node to configure.
 * @pdev: Device requesting the firmware.
 *
 * Returns 0 if successful.
 * Returns <0 for error codes.
 */
int octeon3_pki_cluster_init(int node, struct platform_device *pdev)
{
	const struct firmware *pki_fw;
	const struct fw_hdr *hdr;
	const u64 *data;
	int i, rc;

	rc = request_firmware(&pki_fw, PKI_CLUSTER_FIRMWARE, &pdev->dev);
	if (rc) {
		dev_err(&pdev->dev, "%s: Failed to load %s error=%d\n",
			__FILE__, PKI_CLUSTER_FIRMWARE, rc);
		return rc;
	}

	/* Verify the firmware is valid. */
	hdr = (const struct fw_hdr *)pki_fw->data;
	if ((pki_fw->size - sizeof(const struct fw_hdr) != hdr->size) ||
	    hdr->size % 8) {
		dev_err(&pdev->dev, "%s: Corrupted PKI firmware\n", __FILE__);
		goto err;
	}

	dev_info(&pdev->dev, "%s: Loading PKI firmware %s\n", __FILE__,
		 hdr->version);
	data = hdr->data;
	for (i = 0; i < hdr->size / 8; i++) {
		oct_csr_write(cpu_to_be64(*data), PKI_IMEM(node, i));
		data++;
	}
err:
	release_firmware(pki_fw);

	return 0;
}
EXPORT_SYMBOL(octeon3_pki_cluster_init);

/* octeon3_pki_vlan_init - Configure PCAM to recognize the VLAN ethtypes.
 * @node: Node to configure.
 *
 * Returns 0 if successful.
 * Returns <0 for error codes.
 */
int octeon3_pki_vlan_init(int node)
{
	int i, rc;
	u64 data;

	/* PKI-20858 */
	if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X)) {
		for (i = 0; i < 4; i++) {
			data = oct_csr_read(PKI_CL_ECC_CTL(node, i));
			data &= ~BIT(63);
			data |= BIT(4) | BIT(3);
			oct_csr_write(data, PKI_CL_ECC_CTL(node, i));
		}
	}

	/* Configure the pcam ethtype0 and ethtype1 terms */
	for (i = ETHTYPE0; i <= ETHTYPE1; i++) {
		struct pcam_term_info	term_info;

		/* Term for 0x8100 ethtype */
		term_info.term = i;
		term_info.term_mask = 0xfd;
		term_info.style = 0;
		term_info.style_mask = 0;
		term_info.data = 0x81000000;
		term_info.data_mask = 0xffff0000;
		rc = octeon3_pki_pcam_write_entry(node, &term_info);
		if (rc)
			return rc;

		/* Term for 0x88a8 ethtype */
		term_info.data = 0x88a80000;
		rc = octeon3_pki_pcam_write_entry(node, &term_info);
		if (rc)
			return rc;

		/* Term for 0x9200 ethtype */
		term_info.data = 0x92000000;
		rc = octeon3_pki_pcam_write_entry(node, &term_info);
		if (rc)
			return rc;

		/* Term for 0x9100 ethtype */
		term_info.data = 0x91000000;
		rc = octeon3_pki_pcam_write_entry(node, &term_info);
		if (rc)
			return rc;
	}

	return 0;
}
EXPORT_SYMBOL(octeon3_pki_vlan_init);

/* octeon3_pki_ltype_init - Configures the PKI layer types.
 * @node: Node to configure.
 *
 * Returns 0 if successful.
 * Returns <0 for error codes.
 */
int octeon3_pki_ltype_init(int node)
{
	enum pki_ltype ltype;
	u64 data;
	int i;

	for (i = 0; i < ARRAY_SIZE(dflt_ltype_config); i++) {
		ltype = dflt_ltype_config[i].ltype;
		data = oct_csr_read(PKI_LTYPE_MAP(node, ltype));
		data &= ~GENMASK_ULL(2, 0);
		data |= dflt_ltype_config[i].beltype;
		oct_csr_write(data, PKI_LTYPE_MAP(node, ltype));
	}

	return 0;
}
EXPORT_SYMBOL(octeon3_pki_ltype_init);

int octeon3_pki_srio_init(int node, int pknd)
{
	int i, num_clusters, style;
	u64 data;

	num_clusters = get_num_clusters();
	for (i = 0; i < num_clusters; i++) {
		data = oct_csr_read(PKI_CL_PKIND_STYLE(node, i, pknd));
		style = data & GENMASK_ULL(7, 0);
		data &= ~GENMASK_ULL(14, 8);
		oct_csr_write(data, PKI_CL_PKIND_STYLE(node, i, pknd));

		/* Disable packet length errors and FCS. */
		data = oct_csr_read(PKI_CL_STYLE_CFG(node, i, style));
		data &= ~(BIT(29) | BIT(26) | BIT(25) | BIT(23) | BIT(22));
		oct_csr_write(data, PKI_CL_STYLE_CFG(node, i, style));

		/* Packets have no FCS. */
		data = oct_csr_read(PKI_CL_PKIND_CFG(node, i, pknd));
		data &= ~BIT(7);
		oct_csr_write(data, PKI_CL_PKIND_CFG(node, i, pknd));

		/* Skip the SRIO header and the INST_HDR_S data. */
		data = oct_csr_read(PKI_CL_PKIND_SKIP(node, i, pknd));
		data &= ~(GENMASK_ULL(15, 8) | GENMASK_ULL(7, 0));
		data |= (16 << 8) | 16;
		oct_csr_write(data, PKI_CL_PKIND_SKIP(node, i, pknd));

		/* Exclude port number from Qpg. */
		data = oct_csr_read(PKI_CLX_STYLEX_ALG(node, i, style));
		data &= ~GENMASK_ULL(20, 17);
		oct_csr_write(data, PKI_CLX_STYLEX_ALG(node, i, style));
	}

	return 0;
}
EXPORT_SYMBOL(octeon3_pki_srio_init);

int octeon3_pki_enable(int node)
{
	int timeout;
	u64 data;

	/* Enable backpressure. */
	data = oct_csr_read(PKI_BUF_CTL(node));
	data |= BIT(2);
	oct_csr_write(data, PKI_BUF_CTL(node));

	/* Enable cluster parsing. */
	data = oct_csr_read(PKI_ICG_CFG(node));
	data |= BIT(24);
	oct_csr_write(data, PKI_ICG_CFG(node));

	/* Wait until the PKI is out of reset. */
	timeout = 10000;
	do {
		data = oct_csr_read(PKI_SFT_RST(node));
		if (!(data & BIT(63)))
			break;
		timeout--;
		udelay(1);
	} while (timeout);
	if (!timeout) {
		pr_err("%s: timeout waiting for reset\n", __FILE__);
		return -1;
	}

	/* Enable the PKI. */
	data = oct_csr_read(PKI_BUF_CTL(node));
	data |= BIT(0);
	oct_csr_write(data, PKI_BUF_CTL(node));

	/* Statistics are kept per pknd. */
	oct_csr_write(0, PKI_STAT_CTL(node));

	return 0;
}
EXPORT_SYMBOL(octeon3_pki_enable);

void octeon3_pki_shutdown(int node)
{
	struct global_resource_tag tag;
	int i, j, k, timeout;
	char buf[16];
	u64 data;

	/* Disable the PKI. */
	data = oct_csr_read(PKI_BUF_CTL(node));
	if (data & BIT(0)) {
		data &= ~BIT(0);
		oct_csr_write(data, PKI_BUF_CTL(node));

		/* Wait until the PKI has finished processing packets. */
		timeout = 10000;
		do {
			data = oct_csr_read(PKI_SFT_RST(node));
			if (data & BIT(32))
				break;
			timeout--;
			udelay(1);
		} while (timeout);
		if (!timeout)
			pr_warn("%s: disable timeout\n", __FILE__);
	}

	/* Give all prefetched buffers back to the FPA. */
	data = oct_csr_read(PKI_BUF_CTL(node));
	data |= BIT(5) | BIT(9);
	oct_csr_write(data, PKI_BUF_CTL(node));

	/* Dummy read to get the register write to take effect. */
	data = oct_csr_read(PKI_BUF_CTL(node));

	/* Now we can reset the PKI. */
	data = oct_csr_read(PKI_SFT_RST(node));
	data |= BIT(0);
	oct_csr_write(data, PKI_SFT_RST(node));
	timeout = 10000;
	do {
		data = oct_csr_read(PKI_SFT_RST(node));
		if ((data & BIT(63)) == 0)
			break;
		timeout--;
		udelay(1);
	} while (timeout);
	if (!timeout)
		pr_warn("%s: reset timeout\n", __FILE__);

	/* Free all the allocated resources. */
	for (i = 0; i < PKI_NUM_STYLE; i++) {
		strncpy((char *)&tag.lo, "cvm_styl", 8);
		snprintf(buf, 16, "e_%d.....", node);
		memcpy(&tag.hi, buf, 8);
		res_mgr_free(tag, i);
	}
	for (i = 0; i < PKI_NUM_QPG_ENTRY; i++) {
		strncpy((char *)&tag.lo, "cvm_qpge", 8);
		snprintf(buf, 16, "t_%d.....", node);
		memcpy(&tag.hi, buf, 8);
		res_mgr_free(tag, i);
	}
	for (i = 0; i < get_num_clusters(); i++) {
		for (j = 0; j < MAX_BANKS; j++) {
			strncpy((char *)&tag.lo, "cvm_pcam", 8);
			snprintf(buf, 16, "_%d%d%d....", node, i, j);
			memcpy(&tag.hi, buf, 8);
			for (k = 0; k < MAX_BANK_ENTRIES; k++)
				res_mgr_free(tag, k);
		}
	}

	/* Restore the registers back to their reset state. */
	for (i = 0; i < get_num_clusters(); i++) {
		for (j = 0; j < MAX_PKNDS; j++) {
			oct_csr_write(0, PKI_CL_PKIND_CFG(node, i, j));
			oct_csr_write(0, PKI_CL_PKIND_STYLE(node, i, j));
			oct_csr_write(0, PKI_CL_PKIND_SKIP(node, i, j));
			oct_csr_write(0, PKI_CL_PKIND_L2_CUSTOM(node, i, j));
			oct_csr_write(0, PKI_CL_PKIND_LG_CUSTOM(node, i, j));
		}
		for (j = 0; j < PKI_NUM_FINAL_STYLE; j++) {
			oct_csr_write(0, PKI_CL_STYLE_CFG(node, i, j));
			oct_csr_write(0, PKI_CL_STYLE_CFG2(node, i, j));
			oct_csr_write(0, PKI_CLX_STYLEX_ALG(node, i, j));
		}
	}
	for (i = 0; i < PKI_NUM_FINAL_STYLE; i++)
		oct_csr_write((0x5 << 22) | 0x20, PKI_STYLE_BUF(node, i));
}
EXPORT_SYMBOL(octeon3_pki_shutdown);

MODULE_LICENSE("GPL");
MODULE_FIRMWARE(PKI_CLUSTER_FIRMWARE);
MODULE_AUTHOR("Carlos Munoz <cmunoz@cavium.com>");
MODULE_DESCRIPTION("Octeon III PKI management.");
