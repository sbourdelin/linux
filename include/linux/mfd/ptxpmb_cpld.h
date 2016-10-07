/*---------------------------------------------------------------------------
 *
 * ptxpmb_cpld_core.h
 *     Copyright (c) 2012 Juniper Networks
 *
 *---------------------------------------------------------------------------
 */

#ifndef PTXPMB_CPLD_CORE_H
#define PTXPMB_CPLD_CORE_H

struct pmb_boot_cpld {
	u8 cpld_rev;		/* 0x00 */
	u8 reset;
#define CPLD_MAIN_RESET		(1 << 0)
#define CPLD_PHYCB_RESET	(1 << 1)
#define CPLD_PHYSW_RESET	(1 << 2)	/* P2020 only	*/
#define NGPMB_PCIE_OTHER_RESET	(1 << 3)	/* PAM reset on MLC */
	u8 reset_reason;
#define NGPMB_REASON_MON_A_FAIL	(1 << 0)
#define NGPMB_REASON_WDT1	(1 << 1)
#define NGPMB_REASON_WDT2	(1 << 2)
#define NGPMB_REASON_WDT3	(1 << 3)
#define NGPMB_REASON_WDT4	(1 << 4)
#define NGPMB_REASON_RE_HRST	(1 << 5)
#define NGPMB_REASON_PWR_ON	(1 << 6)
#define NGPMB_REASON_RE_SRST	(1 << 7)
	u8 control;
#define CPLD_CONTROL_BOOTED_LED	(1 << 0)
#define CPLD_CONTROL_WATCHDOG	(1 << 6)
#define CPLD_CONTROL_RTC	(1 << 7)
#define NGPMB_FLASH_SELECT	(1 << 4)
#define NGPMB_FLASH_SWIZZ_ENA	(1 << 5)
	u8 sys_timer_cnt;
	u8 watchdog_hbyte;
	u8 watchdog_lbyte;
	u8 unused1[1];
	u8 baseboard_status1;	/* 0x08 */
#define NGPMB_PMB_STANDALONE	(1 << 0)
#define NGPMB_MASTER_SELECT	(1 << 1)
#define NGPMB_BASEBRD_STANDALONE (1 << 2)
#define NGPMB_BASEBRD_SLOT_LSB	3
#define NGPMB_BASEBRD_SLOT_MASK	0xF8
	u8 baseboard_status2;
#define NGPMB_BASEBRD_TYPE_LSB	5
#define NGPMB_BASEBRD_TYPE_MASK	0xE0
#define NGPMB_BASEBRD_TYPE_MX	0
	u8 chassis_number;
	u8 sys_config;
	u8 i2c_group_sel;	/* 0x0c */
	u8 i2c_group_en;
	u8 unused2[4];
	u8 timer_irq_st;	/* 0x12 */
	u8 timer_irq_en;
	u8 unused3[12];
	u8 prog_jtag_control;	/* 0x20 */
	u8 gp_reset1;		/* 0x21 */
#define CPLD_GP_RST1_PCISW	(1 << 0)
#define CPLD_GP_RST1_SAM	(1 << 1)
#define CPLD_GP_RST1_BRCM	(1 << 2)
	u8 gp_reset2;		/* 0x22 */
	u8 phy_control;
	u8 gpio_1;
	u8 gpio_2;
#define NGPMB_GPIO2_TO_BASEBRD_LSB	(1 << 3)
#define NGPMB_I2C_GRP_SEL_LSB	0
#define NGPMB_I2C_GRP_SEL_MASK	0x03
	u8 thermal_status;
	u8 i2c_host_sel;
#define CPLD_I2C_HOST0_MSTR     0x09
#define CPLD_I2C_HOST1_MSTR     0x06
#define CPLD_I2C_HOST_MSTR_MASK 0x0f
	u8 scratch[3];
	u8 misc_status;
	u8 i2c_bus_control;	/* 0x2c */
	union {
		struct {
			u8 mezz_present;
			u8 unused1[4];
			u8 i2c_group_sel_dbg;	/* 0x31 */
			u8 i2c_group_en_dbg;	/* 0x32 */
			u8 i2c_group_sel_force;	/* 0x33 */
			u8 i2c_group_en_force;	/* 0x34 */
			u8 unused2[0x4b];
		} p2020;
		struct {
			u8 hdk_minor_version;	/* 0x2d */
			u8 hdk_feature_ind;
			u8 hdk_pmb_srds_mode;
			u8 hdk_pwr_fail_status;
			u8 hdk_pmb_pwr_status;
			u8 hdk_pmb_mezz_status;
			u8 cpld_self_reset;	/* 0x33 */
			u8 unused[0x4c];
			u8 hdk_bcpld_rcw[80];
		} p5020;
		struct {
			u8 unused[3];
			u8 chassis_id;		/* 0x30 */
			u8 chassis_type;	/* 0x31 */
#define NGPMB_CHASSIS_TYPE_LSB		0
#define NGPMB_CHASSIS_TYPE_MASK		0x0F
#define NGPMB_CHASSIS_TYPE_POLARIS	0x0B
#define NGPMB_CHASSIS_TYPE_HENDRICKS	0x09
			u8 sys_config;		/* 0x32 */
#define NGPMB_SYS_CONFIG_MULTI_CHASSIS	0x01
		} ngpmb;
		struct {
			u8 nv_win;		/* 0x2d */
			u8 nv_addr1;
			u8 nv_addr2;
			u8 nv_wr_data;
			u8 nv_rd_data;
			u8 nv_cmd;
			u8 nv_done_bit;
		} nvram;
	} board;
};

#ifdef CONFIG_P2020_PTXPMB
#define CPLD_PHY_RESET	(CPLD_PHYCB_RESET | CPLD_PHYSW_RESET)
#else
#define CPLD_PHY_RESET	CPLD_PHYCB_RESET
#endif

#define i2c_group_sel_force board.p2020.i2c_group_sel_force
#define i2c_group_en_force board.p2020.i2c_group_en_force

struct ptxpmb_mux_data {
	int cpld_type;
#define CPLD_TYPE_PTXPMB    0	/* SPMB / Sangria FPC / Hendricks FPC */
#define CPLD_TYPE_NGPMB     1	/* MLC / Stout / Gladiator... */
	int num_enable;		/* Number of I2C enable pins		*/
	int num_channels;	/* Number of I2C channels used in a mux chip */
	int parent_bus_num;	/* parent i2c bus number		*/
	int base_bus_num;	/* 1st bus number, 0 if undefined	*/
	bool use_force;		/* Use i2c force registers if true	*/
};

#endif /* PTXPMB_CPLD_CORE_H */
