/*
 * Copyright (c) 2014 Jes Sorensen <Jes.Sorensen@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * Register definitions taken from original Realttek rtl8723au driver
 */

#include <asm/byteorder.h>

#define RTLMAC_DEBUG_REG_WRITE		1
#define RTLMAC_DEBUG_REG_READ		2
#define RTLMAC_DEBUG_RFREG_WRITE	4
#define RTLMAC_DEBUG_RFREG_READ		8

#define RTW_USB_CONTROL_MSG_TIMEOUT	500
#define RTLMAC_MAX_REG_POLL		500
#define	USB_INTR_CONTENT_LENGTH		56

#define RTLMAC_OUT_ENDPOINTS		3

#define REALTEK_USB_READ		0xc0
#define REALTEK_USB_WRITE		0x40
#define REALTEK_USB_CMD_REQ		0x05
#define REALTEK_USB_CMD_IDX		0x00

#define TX_TOTAL_PAGE_NUM		0xf8
/* (HPQ + LPQ + NPQ + PUBQ) = TX_TOTAL_PAGE_NUM */
#define TX_PAGE_NUM_PUBQ		0xe7
#define TX_PAGE_NUM_HI_PQ		0x0c
#define TX_PAGE_NUM_LO_PQ		0x02
#define TX_PAGE_NUM_NORM_PQ		0x02

#define RTL_FW_PAGE_SIZE		4096
#define RTLMAC_FIRMWARE_POLL_MAX	1000

#define RTL8723A_CHANNEL_GROUPS		3
#define RTL8723A_MAX_RF_PATHS		2
#define RF6052_MAX_TX_PWR		0x3f

#define EFUSE_MAP_LEN_8723A		256
#define EFUSE_MAX_SECTION_8723A		32
#define EFUSE_REAL_CONTENT_LEN_8723A	512
#define EFUSE_BT_MAP_LEN_8723A		1024
#define EFUSE_MAX_WORD_UNIT		4

struct rtlmac_rx_desc {
	u32 pktlen:14;
	u32 crc32:1;
	u32 icverr:1;
	u32 drvinfo_sz:4;
	u32 security:3;
	u32 qos:1;
	u32 shift:2;
	u32 phy_stats:1;
	u32 swdec:1;
	u32 ls:1;
	u32 fs:1;
	u32 eor:1;
	u32 own:1;

	u32 macid:5;
	u32 tid:4;
	u32 hwrsvd:4;
	u32 amsdu:1;
	u32 paggr:1;
	u32 faggr:1;
	u32 a1fit:4;
	u32 a2fit:4;
	u32 pam:1;
	u32 pwr:1;
	u32 md:1;
	u32 mf:1;
	u32 type:2;
	u32 mc:1;
	u32 bc:1;

	u32 seq:12;
	u32 frag:4;
	u32 nextpktlen:14;
	u32 nextind:1;
	u32 reserved0:1;

	u32 rxmcs:6;
	u32 rxht:1;
	u32 gf:1;
	u32 splcp:1;
	u32 bw:1;
	u32 htc:1;
	u32 eosp:1;
	u32 bssidfit:2;
	u32 reserved1:16;
	u32 unicastwake:1;
	u32 magicwake:1;

	u32 pattern0match:1;
	u32 pattern1match:1;
	u32 pattern2match:1;
	u32 pattern3match:1;
	u32 pattern4match:1;
	u32 pattern5match:1;
	u32 pattern6match:1;
	u32 pattern7match:1;
	u32 pattern8match:1;
	u32 pattern9match:1;
	u32 patternamatch:1;
	u32 patternbmatch:1;
	u32 patterncmatch:1;
	u32 reserved2:19;

	u32 tsfl;
#if 0
	u32 bassn:12;
	u32 bavld:1;
	u32 reserved3:19;
#endif
};

struct rtlmac_tx_desc {
	__le16 pkt_size;
	u8 pkt_offset;
	u8 txdw0;
	__le32 txdw1;
	__le32 txdw2;
	__le32 txdw3;
	__le32 txdw4;
	__le32 txdw5;
	__le32 txdw6;
	__le16 csum;
	__le16 txdw7;
};

/*  CCK Rates, TxHT = 0 */
#define TXDESC_RATE_1M			0x00
#define TXDESC_RATE_2M			0x01
#define TXDESC_RATE_5_5M		0x02
#define TXDESC_RATE_11M			0x03

/*  OFDM Rates, TxHT = 0 */
#define TXDESC_RATE_6M			0x04
#define TXDESC_RATE_9M			0x05
#define TXDESC_RATE_12M			0x06
#define TXDESC_RATE_18M			0x07
#define TXDESC_RATE_24M			0x08
#define TXDESC_RATE_36M			0x09
#define TXDESC_RATE_48M			0x0a
#define TXDESC_RATE_54M			0x0b

/*  MCS Rates, TxHT = 1 */
#define TXDESC_RATE_MCS0		0x0c
#define TXDESC_RATE_MCS1		0x0d
#define TXDESC_RATE_MCS2		0x0e
#define TXDESC_RATE_MCS3		0x0f
#define TXDESC_RATE_MCS4		0x10
#define TXDESC_RATE_MCS5		0x11
#define TXDESC_RATE_MCS6		0x12
#define TXDESC_RATE_MCS7		0x13
#define TXDESC_RATE_MCS8		0x14
#define TXDESC_RATE_MCS9		0x15
#define TXDESC_RATE_MCS10		0x16
#define TXDESC_RATE_MCS11		0x17
#define TXDESC_RATE_MCS12		0x18
#define TXDESC_RATE_MCS13		0x19
#define TXDESC_RATE_MCS14		0x1a
#define TXDESC_RATE_MCS15		0x1b
#define TXDESC_RATE_MCS15_SG		0x1c
#define TXDESC_RATE_MCS32		0x20

#define TXDESC_OFFSET_SZ		0
#define TXDESC_OFFSET_SHT		16
#if 0
#define TXDESC_BMC			BIT(24)
#define TXDESC_LSG			BIT(26)
#define TXDESC_FSG			BIT(27)
#define TXDESC_OWN			BIT(31)
#else
#define TXDESC_BROADMULTICAST		BIT(0)
#define TXDESC_LSG			BIT(2)
#define TXDESC_FSG			BIT(3)
#define TXDESC_OWN			BIT(7)
#endif

/* Word 1 */
#define TXDESC_PKT_OFFSET_SZ		0
#define TXDESC_BK			BIT(6)
#define TXDESC_QUEUE_SHIFT		8
#define TXDESC_QUEUE_MASK		0x1f00
#define TXDESC_QUEUE_BK			0x2
#define TXDESC_QUEUE_BE			0x0
#define TXDESC_QUEUE_VI			0x5
#define TXDESC_QUEUE_VO			0x7
#define TXDESC_QUEUE_BEACON		0x10
#define TXDESC_QUEUE_HIGH		0x11
#define TXDESC_QUEUE_MGNT		0x12
#define TXDESC_QUEUE_CMD		0x13
#define TXDESC_QUEUE_MAX		(TXDESC_QUEUE_CMD + 1)

#define TXDESC_RATE_ID_SHIFT		16
#define TXDESC_RATE_ID_MASK		0xf
#define TXDESC_NAVUSEHDR		BIT(20)
#define TXDESC_PKT_OFFSET_SHIFT		26
#define TXDESC_HWPC			BIT(31)

/* Word 2 */
#define TXDESC_AGG_EN			BIT(29)

/* Word 3 */
#define TXDESC_SEQ_SHIFT		16
#define TXDESC_SEQ_MASK			0x000fff0000

/* Word 4 */
#define TXDESC_QOS			BIT(6)
#define TXDESC_HW_SEQ_ENABLE		BIT(7)
#define TXDESC_USE_DRIVER_RATE		BIT(8)
#define TXDESC_DISABLE_DATA_FB		BIT(10)
#define TXDESC_SHORT_PREAMBLE		BIT(24)
#define TXDESC_DATA_BW			BIT(25)

/* Word 5 */
#define TXDESC_RATE_SHIFT		0
#define TXDESC_RATE_MASK		0x3f
#define TXDESC_SHORT_GI			BIT(6)
#define TXDESC_RETRY_LIMIT_ENABLE	BIT(17)
#define TXDESC_RETRY_LIMIT_SHIFT	18
#define TXDESC_RETRY_LIMIT_MASK		0x00ff0000

struct phy_rx_agc_info {
#ifdef __LITTLE_ENDIAN
	u8	gain:7, trsw:1;
#else
	u8	trsw:1, gain:7;
#endif
};

struct rtl8723au_phy_stats {
	struct phy_rx_agc_info path_agc[RTL8723A_MAX_RF_PATHS];
	u8	ch_corr[RTL8723A_MAX_RF_PATHS];
	u8	cck_sig_qual_ofdm_pwdb_all;
	u8	cck_agc_rpt_ofdm_cfosho_a;
	u8	cck_rpt_b_ofdm_cfosho_b;
	u8	reserved_1;
	u8	noise_power_db_msb;
	u8	path_cfotail[RTL8723A_MAX_RF_PATHS];
	u8	pcts_mask[RTL8723A_MAX_RF_PATHS];
	s8	stream_rxevm[RTL8723A_MAX_RF_PATHS];
	u8	path_rxsnr[RTL8723A_MAX_RF_PATHS];
	u8	noise_power_db_lsb;
	u8	reserved_2[3];
	u8	stream_csi[RTL8723A_MAX_RF_PATHS];
	u8	stream_target_csi[RTL8723A_MAX_RF_PATHS];
	s8	sig_evm;
	u8	reserved_3;

#ifdef __LITTLE_ENDIAN
	u8	antsel_rx_keep_2:1;	/* ex_intf_flg:1; */
	u8	sgi_en:1;
	u8	rxsc:2;
	u8	idle_long:1;
	u8	r_ant_train_en:1;
	u8	antenna_select_b:1;
	u8	antenna_select:1;
#else	/*  _BIG_ENDIAN_ */
	u8	antenna_select:1;
	u8	antenna_select_b:1;
	u8	r_ant_train_en:1;
	u8	idle_long:1;
	u8	rxsc:2;
	u8	sgi_en:1;
	u8	antsel_rx_keep_2:1;	/* ex_intf_flg:1; */
#endif
};

/*
 * Regs to backup
 */
#define RTLMAC_ADDA_REGS	16
#define RTLMAC_MAC_REGS		4
#define RTLMAC_BB_REGS		9

struct rtlmac_firmware_header {
	__le16	signature;		/*  92C0: test chip; 92C,
					    88C0: test chip;
					    88C1: MP A-cut;
					    92C1: MP A-cut */
	u8	category;		/*  AP/NIC and USB/PCI */
	u8	function;

	__le16	major_version;		/*  FW Version */
	u8	minor_version;		/*  FW Subversion, default 0x00 */
	u8	reserved1;

	u8	month;			/*  Release time Month field */
	u8	date;			/*  Release time Date field */
	u8	hour;			/*  Release time Hour field */
	u8	minute;			/*  Release time Minute field */

	__le16	ramcodesize;		/*  Size of RAM code */
	u16	reserved2;

	__le32	svn_idx;		/*  SVN entry index */
	u32	reserved3;

	u32	reserved4;
	u32	reserved5;

	u8	data[0];
};

/*
 * The 8723au has 3 channel groups: 1-3, 4-9, and 10-14
 */
struct rtl8723au_idx {
#if defined (__LITTLE_ENDIAN)
	int	a:4;
	int	b:4;
#elif defined (__LITTLE_ENDIAN)
	int	b:4;
	int	a:4;
#else
#error "no endianess defined"
#endif
} __attribute__((packed));

struct rtl8723au_efuse {
	__le16 rtl_id;
	u8 res0[0xe];
	u8 cck_tx_power_index_A[3];	/* 0x10 */
	u8 cck_tx_power_index_B[3];
	u8 ht40_1s_tx_power_index_A[3];	/* 0x16 */
	u8 ht40_1s_tx_power_index_B[3];
	/*
	 * The following entries are half-bytes split as:
	 * bits 0-3: path A, bits 4-7: path B, all values 4 bits signed
	 */
	struct rtl8723au_idx ht20_tx_power_index_diff[3];
	struct rtl8723au_idx ofdm_tx_power_index_diff[3];
	struct rtl8723au_idx ht40_max_power_offset[3];
	struct rtl8723au_idx ht20_max_power_offset[3];
	u8 channel_plan;		/* 0x28 */
	u8 tssi_a;
	u8 thermal_meter;
	u8 rf_regulatory;
	u8 rf_option_2;
	u8 rf_option_3;
	u8 rf_option_4;
	u8 res7;
	u8 version			/* 0x30 */;
	u8 customer_id_major;
	u8 customer_id_minor;
	u8 xtal_k;
	u8 chipset;			/* 0x34 */
	u8 res8[0x82];
	u8 vid;				/* 0xb7 */
	u8 res9;
	u8 pid;				/* 0xb9 */
	u8 res10[0x0c];
	u8 mac_addr[ETH_ALEN];		/* 0xc6 */
	u8 res11[2];
	u8 vendor_name[7];
	u8 res12[2];
	u8 device_name[0x29];		/* 0xd7 */
};

struct rtlmac_reg8val {
	u16 reg;
	u8 val;
};

struct rtlmac_reg32val {
	u16 reg;
	u32 val;
};

struct rtlmac_rfregval {
	u8 reg;
	u32 val;
};

struct rtlmac_priv {
	struct ieee80211_hw *hw;
	struct usb_device *udev;
	u8 mac_addr[ETH_ALEN];
	u32 chip_cut:4;
	u32 rom_rev:4;
	u32 has_wifi:1;
	u32 has_bluetooth:1;
	u32 enable_bluetooth:1;
	u32 has_gps:1;
	u32 vendor_umc:1;
	u32 has_polarity_ctrl:1;
	u32 has_eeprom:1;
	u32 boot_eeprom:1;
	u32 ep_tx_high_queue:1;
	u32 ep_tx_normal_queue:1;
	u32 ep_tx_low_queue:1;
	u32 path_a_hi_power:1;
	u32 path_a_rf_paths:4;
	unsigned int pipe_interrupt;
	unsigned int pipe_in;
	unsigned int pipe_out[TXDESC_QUEUE_MAX];
	u8 out_ep[RTLMAC_OUT_ENDPOINTS];
	u8 path_a_ig_value;
	int ep_tx_count;
	int rf_paths;
	u32 rf_mode_ag[2];
	u32 rege94;
	u32 rege9c;
	u32 regeb4;
	u32 regebc;

	struct urb *rx_urb;
	struct rtlmac_firmware_header *fw_data;
	size_t fw_size;
	struct mutex usb_buf_mutex;
	union {
		__le32 val32;
		__le16 val16;
		u8 val8;
	} usb_buf;
	union {
		u8 raw[EFUSE_MAP_LEN_8723A];
		struct rtl8723au_efuse efuse;
	} efuse_wifi;
	u32 adda_backup[RTLMAC_ADDA_REGS];
	u32 mac_backup[RTLMAC_MAC_REGS];
	u32 bb_backup[RTLMAC_BB_REGS];
	u32 bb_recovery_backup[RTLMAC_BB_REGS];
	u8 pi_enabled:1;
	u8 iqk_initialized:1;
	u8 int_buf[USB_INTR_CONTENT_LENGTH];
};

struct rtlmac_rx_urb
{
	struct urb urb;
	struct ieee80211_hw *hw;
};
