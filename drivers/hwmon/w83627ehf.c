/*
 *  w83627ehf - Driver for the hardware monitoring functionality of
 *		the Winbond W83627EHF Super-I/O chip
 *  Copyright (C) 2017  Peter Huewe <peterhuewe@gmx.de>
 *  Copyright (C) 2005-2012  Jean Delvare <jdelvare@suse.de>
 *  Copyright (C) 2006  Yuan Mu (Winbond),
 *			Rudolf Marek <r.marek@assembler.cz>
 *			David Hubbard <david.c.hubbard@gmail.com>
 *			Daniel J Blueman <daniel.blueman@gmail.com>
 *  Copyright (C) 2010  Sheng-Yuan Huang (Nuvoton) (PS00)
 *
 *  Shamelessly ripped from the w83627hf driver
 *  Copyright (C) 2003  Mark Studebaker
 *
 *  Thanks to Leon Moonen, Steve Cliffe and Grant Coady for their help
 *  in testing and debugging this driver.
 *
 *  This driver also supports the W83627EHG, which is the lead-free
 *  version of the W83627EHF.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  Supports the following chips:
 *
 *  Chip        #vin    #fan    #pwm    #temp  chip IDs       man ID
 *  w83627ehf   10      5       4       3      0x8850 0x88    0x5ca3
 *					       0x8860 0xa1
 *  w83627dhg    9      5       4       3      0xa020 0xc1    0x5ca3
 *  w83627dhg-p  9      5       4       3      0xb070 0xc1    0x5ca3
 *  w83627uhg    8      2       2       3      0xa230 0xc1    0x5ca3
 *  w83667hg     9      5       3       3      0xa510 0xc1    0x5ca3
 *  w83667hg-b   9      5       3       4      0xb350 0xc1    0x5ca3
 *  nct6775f     9      4       3       9      0xb470 0xc1    0x5ca3
 *  nct6776f     9      5       3       9      0xC330 0xc1    0x5ca3
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon-vid.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/acpi.h>
#include <linux/io.h>
#include "lm75.h"

enum kinds {
	w83627ehf, w83627dhg, w83627dhg_p, w83627uhg,
	w83667hg, w83667hg_b, nct6775, nct6776,
};

/* used to set data->name = w83627ehf_device_names[data->sio_kind] */
static const char * const w83627ehf_device_names[] = {
	"w83627ehf",
	"w83627dhg",
	"w83627dhg",
	"w83627uhg",
	"w83667hg",
	"w83667hg",
	"nct6775",
	"nct6776",
};

static unsigned short force_id;
module_param(force_id, ushort, 0);
MODULE_PARM_DESC(force_id, "Override the detected device ID");

static unsigned short fan_debounce;
module_param(fan_debounce, ushort, 0);
MODULE_PARM_DESC(fan_debounce, "Enable debouncing for fan RPM signal");

#define DRVNAME "w83627ehf"

/*
 * Super-I/O constants and functions
 */

#define W83627EHF_LD_HWM	0x0b
#define W83667HG_LD_VID		0x0d

#define SIO_REG_LDSEL		0x07	/* Logical device select */
#define SIO_REG_DEVID		0x20	/* Device ID (2 bytes) */
#define SIO_REG_EN_VRM10	0x2C	/* GPIO3, GPIO4 selection */
#define SIO_REG_ENABLE		0x30	/* Logical device enable */
#define SIO_REG_ADDR		0x60	/* Logical device address (2 bytes) */
#define SIO_REG_VID_CTRL	0xF0	/* VID control */
#define SIO_REG_VID_DATA	0xF1	/* VID data */

#define SIO_W83627EHF_ID	0x8850
#define SIO_W83627EHG_ID	0x8860
#define SIO_W83627DHG_ID	0xa020
#define SIO_W83627DHG_P_ID	0xb070
#define SIO_W83627UHG_ID	0xa230
#define SIO_W83667HG_ID		0xa510
#define SIO_W83667HG_B_ID	0xb350
#define SIO_NCT6775_ID		0xb470
#define SIO_NCT6776_ID		0xc330
#define SIO_ID_MASK		0xFFF0

static inline void
superio_outb(int ioreg, int reg, int val)
{
	outb(reg, ioreg);
	outb(val, ioreg + 1);
}

static inline int
superio_inb(int ioreg, int reg)
{
	outb(reg, ioreg);
	return inb(ioreg + 1);
}

static inline void
superio_select(int ioreg, int ld)
{
	outb(SIO_REG_LDSEL, ioreg);
	outb(ld, ioreg + 1);
}

static inline int
superio_enter(int ioreg)
{
	if (!request_muxed_region(ioreg, 2, DRVNAME))
		return -EBUSY;

	outb(0x87, ioreg);
	outb(0x87, ioreg);

	return 0;
}

static inline void
superio_exit(int ioreg)
{
	outb(0xaa, ioreg);
	outb(0x02, ioreg);
	outb(0x02, ioreg + 1);
	release_region(ioreg, 2);
}

/*
 * ISA constants
 */

#define IOREGION_ALIGNMENT	(~7)
#define IOREGION_OFFSET		5
#define IOREGION_LENGTH		2
#define ADDR_REG_OFFSET		0
#define DATA_REG_OFFSET		1

#define W83627EHF_REG_BANK		0x4E
#define W83627EHF_REG_CONFIG		0x40

/*
 * Not currently used:
 * REG_MAN_ID has the value 0x5ca3 for all supported chips.
 * REG_CHIP_ID == 0x88/0xa1/0xc1 depending on chip model.
 * REG_MAN_ID is at port 0x4f
 * REG_CHIP_ID is at port 0x58
 */

static const u16 W83627EHF_REG_FAN[] = { 0x28, 0x29, 0x2a, 0x3f, 0x553 };
static const u16 W83627EHF_REG_FAN_MIN[] = { 0x3b, 0x3c, 0x3d, 0x3e, 0x55c };

/* The W83627EHF registers for nr=7,8,9 are in bank 5 */
#define W83627EHF_REG_IN_MAX(nr)	((nr < 7) ? (0x2b + (nr) * 2) : \
					 (0x554 + (((nr) - 7) * 2)))
#define W83627EHF_REG_IN_MIN(nr)	((nr < 7) ? (0x2c + (nr) * 2) : \
					 (0x555 + (((nr) - 7) * 2)))
#define W83627EHF_REG_IN(nr)		((nr < 7) ? (0x20 + (nr)) : \
					 (0x550 + (nr) - 7))

static const u16 W83627EHF_REG_TEMP[] = { 0x27, 0x150, 0x250, 0x7e };
static const u16 W83627EHF_REG_TEMP_HYST[] = { 0x3a, 0x153, 0x253, 0 };
static const u16 W83627EHF_REG_TEMP_OVER[] = { 0x39, 0x155, 0x255, 0 };
static const u16 W83627EHF_REG_TEMP_CONFIG[] = { 0, 0x152, 0x252, 0 };

/* Fan clock dividers are spread over the following five registers */
#define W83627EHF_REG_FANDIV1		0x47
#define W83627EHF_REG_FANDIV2		0x4B
#define W83627EHF_REG_VBAT		0x5D
#define W83627EHF_REG_DIODE		0x59
#define W83627EHF_REG_SMI_OVT		0x4C

/* NCT6775F has its own fan divider registers */
#define NCT6775_REG_FANDIV1		0x506
#define NCT6775_REG_FANDIV2		0x507
#define NCT6775_REG_FAN_DEBOUNCE	0xf0

#define W83627EHF_REG_ALARM1		0x459
#define W83627EHF_REG_ALARM2		0x45A
#define W83627EHF_REG_ALARM3		0x45B

#define W83627EHF_REG_CASEOPEN_DET	0x42 /* SMI STATUS #2 */
#define W83627EHF_REG_CASEOPEN_CLR	0x46 /* SMI MASK #3 */

/* SmartFan registers */
#define W83627EHF_REG_FAN_STEPUP_TIME 0x0f
#define W83627EHF_REG_FAN_STEPDOWN_TIME 0x0e

/* DC or PWM output fan configuration */
static const u8 W83627EHF_REG_PWM_ENABLE[] = {
	0x04,			/* SYS FAN0 output mode and PWM mode */
	0x04,			/* CPU FAN0 output mode and PWM mode */
	0x12,			/* AUX FAN mode */
	0x62,			/* CPU FAN1 mode */
};

static const u8 W83627EHF_PWM_MODE_SHIFT[] = { 0, 1, 0, 6 };
static const u8 W83627EHF_PWM_ENABLE_SHIFT[] = { 2, 4, 1, 4 };

/* FAN Duty Cycle, be used to control */
static const u16 W83627EHF_REG_PWM[] = { 0x01, 0x03, 0x11, 0x61 };
static const u16 W83627EHF_REG_TARGET[] = { 0x05, 0x06, 0x13, 0x63 };
static const u8 W83627EHF_REG_TOLERANCE[] = { 0x07, 0x07, 0x14, 0x62 };

/* Advanced Fan control, some values are common for all fans */
static const u16 W83627EHF_REG_FAN_START_OUTPUT[] = { 0x0a, 0x0b, 0x16, 0x65 };
static const u16 W83627EHF_REG_FAN_STOP_OUTPUT[] = { 0x08, 0x09, 0x15, 0x64 };
static const u16 W83627EHF_REG_FAN_STOP_TIME[] = { 0x0c, 0x0d, 0x17, 0x66 };

static const u16 W83627EHF_REG_FAN_MAX_OUTPUT_COMMON[]
						= { 0xff, 0x67, 0xff, 0x69 };
static const u16 W83627EHF_REG_FAN_STEP_OUTPUT_COMMON[]
						= { 0xff, 0x68, 0xff, 0x6a };

static const u16 W83627EHF_REG_FAN_MAX_OUTPUT_W83667_B[] = { 0x67, 0x69, 0x6b };
static const u16 W83627EHF_REG_FAN_STEP_OUTPUT_W83667_B[]
						= { 0x68, 0x6a, 0x6c };

static const u16 W83627EHF_REG_TEMP_OFFSET[] = { 0x454, 0x455, 0x456 };

static const u16 NCT6775_REG_TARGET[] = { 0x101, 0x201, 0x301 };
static const u16 NCT6775_REG_FAN_MODE[] = { 0x102, 0x202, 0x302 };
static const u16 NCT6775_REG_FAN_STOP_OUTPUT[] = { 0x105, 0x205, 0x305 };
static const u16 NCT6775_REG_FAN_START_OUTPUT[] = { 0x106, 0x206, 0x306 };
static const u16 NCT6775_REG_FAN_STOP_TIME[] = { 0x107, 0x207, 0x307 };
static const u16 NCT6775_REG_PWM[] = { 0x109, 0x209, 0x309 };
static const u16 NCT6775_REG_FAN_MAX_OUTPUT[] = { 0x10a, 0x20a, 0x30a };
static const u16 NCT6775_REG_FAN_STEP_OUTPUT[] = { 0x10b, 0x20b, 0x30b };
static const u16 NCT6775_REG_FAN[] = { 0x630, 0x632, 0x634, 0x636, 0x638 };
static const u16 NCT6776_REG_FAN_MIN[] = { 0x63a, 0x63c, 0x63e, 0x640, 0x642};

static const u16 NCT6775_REG_TEMP[]
	= { 0x27, 0x150, 0x250, 0x73, 0x75, 0x77, 0x62b, 0x62c, 0x62d };
static const u16 NCT6775_REG_TEMP_CONFIG[]
	= { 0, 0x152, 0x252, 0, 0, 0, 0x628, 0x629, 0x62A };
static const u16 NCT6775_REG_TEMP_HYST[]
	= { 0x3a, 0x153, 0x253, 0, 0, 0, 0x673, 0x678, 0x67D };
static const u16 NCT6775_REG_TEMP_OVER[]
	= { 0x39, 0x155, 0x255, 0, 0, 0, 0x672, 0x677, 0x67C };
static const u16 NCT6775_REG_TEMP_SOURCE[]
	= { 0x621, 0x622, 0x623, 0x100, 0x200, 0x300, 0x624, 0x625, 0x626 };

static const char *const w83667hg_b_temp_label[] = {
	"SYSTIN",
	"CPUTIN",
	"AUXTIN",
	"AMDTSI",
	"PECI Agent 1",
	"PECI Agent 2",
	"PECI Agent 3",
	"PECI Agent 4"
};

static const char *const nct6775_temp_label[] = {
	"",
	"SYSTIN",
	"CPUTIN",
	"AUXTIN",
	"AMD SB-TSI",
	"PECI Agent 0",
	"PECI Agent 1",
	"PECI Agent 2",
	"PECI Agent 3",
	"PECI Agent 4",
	"PECI Agent 5",
	"PECI Agent 6",
	"PECI Agent 7",
	"PCH_CHIP_CPU_MAX_TEMP",
	"PCH_CHIP_TEMP",
	"PCH_CPU_TEMP",
	"PCH_MCH_TEMP",
	"PCH_DIM0_TEMP",
	"PCH_DIM1_TEMP",
	"PCH_DIM2_TEMP",
	"PCH_DIM3_TEMP"
};

static const char *const nct6776_temp_label[] = {
	"",
	"SYSTIN",
	"CPUTIN",
	"AUXTIN",
	"SMBUSMASTER 0",
	"SMBUSMASTER 1",
	"SMBUSMASTER 2",
	"SMBUSMASTER 3",
	"SMBUSMASTER 4",
	"SMBUSMASTER 5",
	"SMBUSMASTER 6",
	"SMBUSMASTER 7",
	"PECI Agent 0",
	"PECI Agent 1",
	"PCH_CHIP_CPU_MAX_TEMP",
	"PCH_CHIP_TEMP",
	"PCH_CPU_TEMP",
	"PCH_MCH_TEMP",
	"PCH_DIM0_TEMP",
	"PCH_DIM1_TEMP",
	"PCH_DIM2_TEMP",
	"PCH_DIM3_TEMP",
	"BYTE_TEMP"
};

#define NUM_REG_TEMP	ARRAY_SIZE(NCT6775_REG_TEMP)

static int is_word_sized(u16 reg)
{
	return ((((reg & 0xff00) == 0x100
	      || (reg & 0xff00) == 0x200)
	     && ((reg & 0x00ff) == 0x50
	      || (reg & 0x00ff) == 0x53
	      || (reg & 0x00ff) == 0x55))
	     || (reg & 0xfff0) == 0x630
	     || reg == 0x640 || reg == 0x642
	     || ((reg & 0xfff0) == 0x650
		 && (reg & 0x000f) >= 0x06)
	     || reg == 0x73 || reg == 0x75 || reg == 0x77
		);
}

/*
 * Conversions
 */

/* 1 is PWM mode, output in ms */
static inline unsigned int step_time_from_reg(u8 reg, u8 mode)
{
	return mode ? 100 * reg : 400 * reg;
}

static inline u8 step_time_to_reg(unsigned int msec, u8 mode)
{
	return clamp_val((mode ? (msec + 50) / 100 : (msec + 200) / 400),
			 1, 255);
}

static unsigned int fan_from_reg8(u16 reg, unsigned int divreg)
{
	if (reg == 0 || reg == 255)
		return 0;
	return 1350000U / (reg << divreg);
}

static unsigned int fan_from_reg13(u16 reg, unsigned int divreg)
{
	if ((reg & 0xff1f) == 0xff1f)
		return 0;

	reg = (reg & 0x1f) | ((reg & 0xff00) >> 3);

	if (reg == 0)
		return 0;

	return 1350000U / reg;
}

static unsigned int fan_from_reg16(u16 reg, unsigned int divreg)
{
	if (reg == 0 || reg == 0xffff)
		return 0;

	/*
	 * Even though the registers are 16 bit wide, the fan divisor
	 * still applies.
	 */
	return 1350000U / (reg << divreg);
}

static inline unsigned int
div_from_reg(u8 reg)
{
	return 1 << reg;
}

/*
 * Some of the voltage inputs have internal scaling, the tables below
 * contain 8 (the ADC LSB in mV) * scaling factor * 100
 */
static const u16 scale_in_common[10] = {
	800, 800, 1600, 1600, 800, 800, 800, 1600, 1600, 800
};

static const u16 scale_in_w83627uhg[9] = {
	800, 800, 3328, 3424, 800, 800, 0, 3328, 3400
};

static inline long in_from_reg(u8 reg, u8 nr, const u16 *scale_in)
{
	return DIV_ROUND_CLOSEST(reg * scale_in[nr], 100);
}

static inline u8 in_to_reg(u32 val, u8 nr, const u16 *scale_in)
{
	return clamp_val(DIV_ROUND_CLOSEST(val * 100, scale_in[nr]), 0, 255);
}

/*
 * Data structures and manipulation thereof
 */
struct w83627ehf_sio_data {
	int sioreg;
	enum kinds kind;
};

struct w83627ehf_data {
	int addr;	/* IO base of hw monitor block */
	const char *name;

	struct device *hwmon_dev;
	struct mutex lock;

	u16 reg_temp[NUM_REG_TEMP];
	u16 reg_temp_over[NUM_REG_TEMP];
	u16 reg_temp_hyst[NUM_REG_TEMP];
	u16 reg_temp_config[NUM_REG_TEMP];
	u8 temp_src[NUM_REG_TEMP];
	const char * const *temp_label;

	const u16 *REG_PWM;
	const u16 *REG_TARGET;
	const u16 *REG_FAN;
	const u16 *REG_FAN_MIN;
	const u16 *REG_FAN_START_OUTPUT;
	const u16 *REG_FAN_STOP_OUTPUT;
	const u16 *REG_FAN_STOP_TIME;
	const u16 *REG_FAN_MAX_OUTPUT;
	const u16 *REG_FAN_STEP_OUTPUT;
	const u16 *scale_in;

	unsigned int (*fan_from_reg)(u16 reg, unsigned int divreg);
	unsigned int (*fan_from_reg_min)(u16 reg, unsigned int divreg);

	struct mutex update_lock;
	char valid;		/* !=0 if following fields are valid */
	unsigned long last_updated;	/* In jiffies */

	/* Register values */
	u8 bank;		/* current register bank */
	u8 in_num;		/* number of in inputs we have */
	u8 in[10];		/* Register value */
	u8 in_max[10];		/* Register value */
	u8 in_min[10];		/* Register value */
	unsigned int rpm[5];
	u16 fan_min[5];
	u8 fan_div[5];
	u8 has_fan;		/* some fan inputs can be disabled */
	u8 has_fan_min;		/* some fans don't have min register */
	bool has_fan_div;
	u8 temp_type[3];
	s8 temp_offset[3];
	s16 temp[9];
	s16 temp_max[9];
	s16 temp_max_hyst[9];
	u32 alarms;
	u8 caseopen;

	u8 pwm_mode[4]; /* 0->DC variable voltage, 1->PWM variable duty cycle */
	u8 pwm_enable[4]; /* 1->manual
			   * 2->thermal cruise mode (also called SmartFan I)
			   * 3->fan speed cruise mode
			   * 4->variable thermal cruise (also called
			   * SmartFan III)
			   * 5->enhanced variable thermal cruise (also called
			   * SmartFan IV)
			   */
	u8 pwm_enable_orig[4];	/* original value of pwm_enable */
	u8 pwm_num;		/* number of pwm */
	u8 pwm[4];
	u8 target_temp[4];
	u8 tolerance[4];

	u8 fan_start_output[4]; /* minimum fan speed when spinning up */
	u8 fan_stop_output[4]; /* minimum fan speed when spinning down */
	u8 fan_stop_time[4]; /* time at minimum before disabling fan */
	u8 fan_max_output[4]; /* maximum fan speed */
	u8 fan_step_output[4]; /* rate of change output value */

	u8 vid;
	u8 vrm;

	u16 have_temp;
	u16 have_temp_offset;
	u8 in6_skip:1;
	u8 temp3_val_only:1;

#ifdef CONFIG_PM
	/* Remember extra register values over suspend/resume */
	u8 vbat;
	u8 fandiv1;
	u8 fandiv2;
#endif
	struct w83627ehf_sio_data *sio_data;
};

/*
 * On older chips, only registers 0x50-0x5f are banked.
 * On more recent chips, all registers are banked.
 * Assume that is the case and set the bank number for each access.
 * Cache the bank number so it only needs to be set if it changes.
 */
static inline void w83627ehf_set_bank(struct w83627ehf_data *data, u16 reg)
{
	u8 bank = reg >> 8;

	if (data->bank != bank) {
		outb_p(W83627EHF_REG_BANK, data->addr + ADDR_REG_OFFSET);
		outb_p(bank, data->addr + DATA_REG_OFFSET);
		data->bank = bank;
	}
}

static u16 w83627ehf_read_value(struct w83627ehf_data *data, u16 reg)
{
	int res, word_sized = is_word_sized(reg);

	mutex_lock(&data->lock);

	w83627ehf_set_bank(data, reg);
	outb_p(reg & 0xff, data->addr + ADDR_REG_OFFSET);
	res = inb_p(data->addr + DATA_REG_OFFSET);
	if (word_sized) {
		outb_p((reg & 0xff) + 1, data->addr + ADDR_REG_OFFSET);
		res = (res << 8) + inb_p(data->addr + DATA_REG_OFFSET);
	}

	mutex_unlock(&data->lock);
	return res;
}

static int w83627ehf_write_value(struct w83627ehf_data *data, u16 reg,
				 u16 value)
{
	int word_sized = is_word_sized(reg);

	mutex_lock(&data->lock);

	w83627ehf_set_bank(data, reg);
	outb_p(reg & 0xff, data->addr + ADDR_REG_OFFSET);
	if (word_sized) {
		outb_p(value >> 8, data->addr + DATA_REG_OFFSET);
		outb_p((reg & 0xff) + 1, data->addr + ADDR_REG_OFFSET);
	}
	outb_p(value & 0xff, data->addr + DATA_REG_OFFSET);

	mutex_unlock(&data->lock);
	return 0;
}

/* We left-align 8-bit temperature values to make the code simpler */
static u16 w83627ehf_read_temp(struct w83627ehf_data *data, u16 reg)
{
	u16 res;

	res = w83627ehf_read_value(data, reg);
	if (!is_word_sized(reg))
		res <<= 8;

	return res;
}

static int w83627ehf_write_temp(struct w83627ehf_data *data, u16 reg, u16 value)
{
	if (!is_word_sized(reg))
		value >>= 8;
	return w83627ehf_write_value(data, reg, value);
}

/* This function assumes that the caller holds data->update_lock */
static void nct6775_write_fan_div(struct w83627ehf_data *data, int nr)
{
	u8 reg;

	switch (nr) {
	case 0:
		reg = (w83627ehf_read_value(data, NCT6775_REG_FANDIV1) & 0x70)
		    | (data->fan_div[0] & 0x7);
		w83627ehf_write_value(data, NCT6775_REG_FANDIV1, reg);
		break;
	case 1:
		reg = (w83627ehf_read_value(data, NCT6775_REG_FANDIV1) & 0x7)
		    | ((data->fan_div[1] << 4) & 0x70);
		w83627ehf_write_value(data, NCT6775_REG_FANDIV1, reg);
		break;
	case 2:
		reg = (w83627ehf_read_value(data, NCT6775_REG_FANDIV2) & 0x70)
		    | (data->fan_div[2] & 0x7);
		w83627ehf_write_value(data, NCT6775_REG_FANDIV2, reg);
		break;
	case 3:
		reg = (w83627ehf_read_value(data, NCT6775_REG_FANDIV2) & 0x7)
		    | ((data->fan_div[3] << 4) & 0x70);
		w83627ehf_write_value(data, NCT6775_REG_FANDIV2, reg);
		break;
	}
}

/* This function assumes that the caller holds data->update_lock */
static void w83627ehf_write_fan_div(struct w83627ehf_data *data, int nr)
{
	u8 reg;

	switch (nr) {
	case 0:
		reg = (w83627ehf_read_value(data, W83627EHF_REG_FANDIV1) & 0xcf)
		    | ((data->fan_div[0] & 0x03) << 4);
		/* fan5 input control bit is write only, compute the value */
		reg |= (data->has_fan & (1 << 4)) ? 1 : 0;
		w83627ehf_write_value(data, W83627EHF_REG_FANDIV1, reg);
		reg = (w83627ehf_read_value(data, W83627EHF_REG_VBAT) & 0xdf)
		    | ((data->fan_div[0] & 0x04) << 3);
		w83627ehf_write_value(data, W83627EHF_REG_VBAT, reg);
		break;
	case 1:
		reg = (w83627ehf_read_value(data, W83627EHF_REG_FANDIV1) & 0x3f)
		    | ((data->fan_div[1] & 0x03) << 6);
		/* fan5 input control bit is write only, compute the value */
		reg |= (data->has_fan & (1 << 4)) ? 1 : 0;
		w83627ehf_write_value(data, W83627EHF_REG_FANDIV1, reg);
		reg = (w83627ehf_read_value(data, W83627EHF_REG_VBAT) & 0xbf)
		    | ((data->fan_div[1] & 0x04) << 4);
		w83627ehf_write_value(data, W83627EHF_REG_VBAT, reg);
		break;
	case 2:
		reg = (w83627ehf_read_value(data, W83627EHF_REG_FANDIV2) & 0x3f)
		    | ((data->fan_div[2] & 0x03) << 6);
		w83627ehf_write_value(data, W83627EHF_REG_FANDIV2, reg);
		reg = (w83627ehf_read_value(data, W83627EHF_REG_VBAT) & 0x7f)
		    | ((data->fan_div[2] & 0x04) << 5);
		w83627ehf_write_value(data, W83627EHF_REG_VBAT, reg);
		break;
	case 3:
		reg = (w83627ehf_read_value(data, W83627EHF_REG_DIODE) & 0xfc)
		    | (data->fan_div[3] & 0x03);
		w83627ehf_write_value(data, W83627EHF_REG_DIODE, reg);
		reg = (w83627ehf_read_value(data, W83627EHF_REG_SMI_OVT) & 0x7f)
		    | ((data->fan_div[3] & 0x04) << 5);
		w83627ehf_write_value(data, W83627EHF_REG_SMI_OVT, reg);
		break;
	case 4:
		reg = (w83627ehf_read_value(data, W83627EHF_REG_DIODE) & 0x73)
		    | ((data->fan_div[4] & 0x03) << 2)
		    | ((data->fan_div[4] & 0x04) << 5);
		w83627ehf_write_value(data, W83627EHF_REG_DIODE, reg);
		break;
	}
}

static void w83627ehf_write_fan_div_common(struct device *dev,
					   struct w83627ehf_data *data, int nr)
{
	struct w83627ehf_sio_data *sio_data = data->sio_data;

	if (sio_data->kind == nct6776)
		; /* no dividers, do nothing */
	else if (sio_data->kind == nct6775)
		nct6775_write_fan_div(data, nr);
	else
		w83627ehf_write_fan_div(data, nr);
}

static void nct6775_update_fan_div(struct w83627ehf_data *data)
{
	u8 i;

	i = w83627ehf_read_value(data, NCT6775_REG_FANDIV1);
	data->fan_div[0] = i & 0x7;
	data->fan_div[1] = (i & 0x70) >> 4;
	i = w83627ehf_read_value(data, NCT6775_REG_FANDIV2);
	data->fan_div[2] = i & 0x7;
	if (data->has_fan & (1 << 3))
		data->fan_div[3] = (i & 0x70) >> 4;
}

static void w83627ehf_update_fan_div(struct w83627ehf_data *data)
{
	int i;

	i = w83627ehf_read_value(data, W83627EHF_REG_FANDIV1);
	data->fan_div[0] = (i >> 4) & 0x03;
	data->fan_div[1] = (i >> 6) & 0x03;
	i = w83627ehf_read_value(data, W83627EHF_REG_FANDIV2);
	data->fan_div[2] = (i >> 6) & 0x03;
	i = w83627ehf_read_value(data, W83627EHF_REG_VBAT);
	data->fan_div[0] |= (i >> 3) & 0x04;
	data->fan_div[1] |= (i >> 4) & 0x04;
	data->fan_div[2] |= (i >> 5) & 0x04;
	if (data->has_fan & ((1 << 3) | (1 << 4))) {
		i = w83627ehf_read_value(data, W83627EHF_REG_DIODE);
		data->fan_div[3] = i & 0x03;
		data->fan_div[4] = ((i >> 2) & 0x03)
				 | ((i >> 5) & 0x04);
	}
	if (data->has_fan & (1 << 3)) {
		i = w83627ehf_read_value(data, W83627EHF_REG_SMI_OVT);
		data->fan_div[3] |= (i >> 5) & 0x04;
	}
}

static void w83627ehf_update_fan_div_common(struct device *dev,
					    struct w83627ehf_data *data)
{
	if (data->sio_data->kind == nct6776)
		; /* no dividers, do nothing */
	else if (data->sio_data->kind == nct6775)
		nct6775_update_fan_div(data);
	else
		w83627ehf_update_fan_div(data);
}

static void nct6775_update_pwm(struct w83627ehf_data *data)
{
	int i;
	int pwmcfg, fanmodecfg;

	for (i = 0; i < data->pwm_num; i++) {
		pwmcfg = w83627ehf_read_value(data,
					      W83627EHF_REG_PWM_ENABLE[i]);
		fanmodecfg = w83627ehf_read_value(data,
						  NCT6775_REG_FAN_MODE[i]);
		data->pwm_mode[i] =
		  ((pwmcfg >> W83627EHF_PWM_MODE_SHIFT[i]) & 1) ? 0 : 1;
		data->pwm_enable[i] = ((fanmodecfg >> 4) & 7) + 1;
		data->tolerance[i] = fanmodecfg & 0x0f;
		data->pwm[i] = w83627ehf_read_value(data, data->REG_PWM[i]);
	}
}

static void w83627ehf_update_pwm(struct w83627ehf_data *data)
{
	int i;
	int pwmcfg = 0, tolerance = 0; /* shut up the compiler */

	for (i = 0; i < data->pwm_num; i++) {
		if (!(data->has_fan & (1 << i)))
			continue;

		/* pwmcfg, tolerance mapped for i=0, i=1 to same reg */
		if (i != 1) {
			pwmcfg = w83627ehf_read_value(data,
					W83627EHF_REG_PWM_ENABLE[i]);
			tolerance = w83627ehf_read_value(data,
					W83627EHF_REG_TOLERANCE[i]);
		}
		data->pwm_mode[i] =
			((pwmcfg >> W83627EHF_PWM_MODE_SHIFT[i]) & 1) ? 0 : 1;
		data->pwm_enable[i] = ((pwmcfg >> W83627EHF_PWM_ENABLE_SHIFT[i])
				       & 3) + 1;
		data->pwm[i] = w83627ehf_read_value(data, data->REG_PWM[i]);

		data->tolerance[i] = (tolerance >> (i == 1 ? 4 : 0)) & 0x0f;
	}
}

static void w83627ehf_update_pwm_common(struct device *dev,
					struct w83627ehf_data *data)
{
	struct w83627ehf_sio_data *sio_data = data->sio_data;

	if (sio_data->kind == nct6775 || sio_data->kind == nct6776)
		nct6775_update_pwm(data);
	else
		w83627ehf_update_pwm(data);
}

static struct w83627ehf_data *w83627ehf_update_device(struct device *dev)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);
	struct w83627ehf_sio_data *sio_data = data->sio_data;
	int i;

	mutex_lock(&data->update_lock);

	if (time_after(jiffies, data->last_updated + HZ + HZ/2)
	 || !data->valid) {
		/* Fan clock dividers */
		w83627ehf_update_fan_div_common(dev, data);

		/* Measured voltages and limits */
		for (i = 0; i < data->in_num; i++) {
			if ((i == 6) && data->in6_skip)
				continue;

			data->in[i] = w83627ehf_read_value(data,
				      W83627EHF_REG_IN(i));
			data->in_min[i] = w83627ehf_read_value(data,
					  W83627EHF_REG_IN_MIN(i));
			data->in_max[i] = w83627ehf_read_value(data,
					  W83627EHF_REG_IN_MAX(i));
		}

		/* Measured fan speeds and limits */
		for (i = 0; i < 5; i++) {
			u16 reg;

			if (!(data->has_fan & (1 << i)))
				continue;

			reg = w83627ehf_read_value(data, data->REG_FAN[i]);
			data->rpm[i] = data->fan_from_reg(reg,
							  data->fan_div[i]);

			if (data->has_fan_min & (1 << i))
				data->fan_min[i] = w83627ehf_read_value(data,
					   data->REG_FAN_MIN[i]);

			/*
			 * If we failed to measure the fan speed and clock
			 * divider can be increased, let's try that for next
			 * time
			 */
			if (data->has_fan_div
			    && (reg >= 0xff || (sio_data->kind == nct6775
						&& reg == 0x00))
			    && data->fan_div[i] < 0x07) {
				dev_dbg(dev,
					"Increasing fan%d clock divider from %u to %u\n",
					i + 1, div_from_reg(data->fan_div[i]),
					div_from_reg(data->fan_div[i] + 1));
				data->fan_div[i]++;
				w83627ehf_write_fan_div_common(dev, data, i);
				/* Preserve min limit if possible */
				if ((data->has_fan_min & (1 << i))
				 && data->fan_min[i] >= 2
				 && data->fan_min[i] != 255)
					w83627ehf_write_value(data,
						data->REG_FAN_MIN[i],
						(data->fan_min[i] /= 2));
			}
		}

		w83627ehf_update_pwm_common(dev, data);

		for (i = 0; i < data->pwm_num; i++) {
			if (!(data->has_fan & (1 << i)))
				continue;

			data->fan_start_output[i] =
			  w83627ehf_read_value(data,
					       data->REG_FAN_START_OUTPUT[i]);
			data->fan_stop_output[i] =
			  w83627ehf_read_value(data,
					       data->REG_FAN_STOP_OUTPUT[i]);
			data->fan_stop_time[i] =
			  w83627ehf_read_value(data,
					       data->REG_FAN_STOP_TIME[i]);

			if (data->REG_FAN_MAX_OUTPUT &&
			    data->REG_FAN_MAX_OUTPUT[i] != 0xff)
				data->fan_max_output[i] =
				  w83627ehf_read_value(data,
						data->REG_FAN_MAX_OUTPUT[i]);

			if (data->REG_FAN_STEP_OUTPUT &&
			    data->REG_FAN_STEP_OUTPUT[i] != 0xff)
				data->fan_step_output[i] =
				  w83627ehf_read_value(data,
						data->REG_FAN_STEP_OUTPUT[i]);

			data->target_temp[i] =
				w83627ehf_read_value(data,
					data->REG_TARGET[i]) &
					(data->pwm_mode[i] == 1 ? 0x7f : 0xff);
		}

		/* Measured temperatures and limits */
		for (i = 0; i < NUM_REG_TEMP; i++) {
			if (!(data->have_temp & (1 << i)))
				continue;
			data->temp[i] = w83627ehf_read_temp(data,
						data->reg_temp[i]);
			if (data->reg_temp_over[i])
				data->temp_max[i]
				  = w83627ehf_read_temp(data,
						data->reg_temp_over[i]);
			if (data->reg_temp_hyst[i])
				data->temp_max_hyst[i]
				  = w83627ehf_read_temp(data,
						data->reg_temp_hyst[i]);
			if (i > 2)
				continue;
			if (data->have_temp_offset & (1 << i))
				data->temp_offset[i]
				  = w83627ehf_read_value(data,
						W83627EHF_REG_TEMP_OFFSET[i]);
		}

		data->alarms = w83627ehf_read_value(data,
					W83627EHF_REG_ALARM1) |
			       (w83627ehf_read_value(data,
					W83627EHF_REG_ALARM2) << 8) |
			       (w83627ehf_read_value(data,
					W83627EHF_REG_ALARM3) << 16);

		data->caseopen = w83627ehf_read_value(data,
						W83627EHF_REG_CASEOPEN_DET);

		data->last_updated = jiffies;
		data->valid = 1;
	}

	mutex_unlock(&data->update_lock);
	return data;
}

static void store_fan_min(struct device *dev, u32 channel, unsigned long val)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);
	int nr = channel;
	unsigned int reg;
	u8 new_div;

	mutex_lock(&data->update_lock);
	if (!data->has_fan_div) {
		/*
		 * Only NCT6776F for now, so we know that this is a 13 bit
		 * register
		 */
		if (!val) {
			val = 0xff1f;
		} else {
			if (val > 1350000U)
				val = 135000U;
			val = 1350000U / val;
			val = (val & 0x1f) | ((val << 3) & 0xff00);
		}
		data->fan_min[nr] = val;
		goto done;	/* Leave fan divider alone */
	}
	if (!val) {
		/* No min limit, alarm disabled */
		data->fan_min[nr] = 255;
		new_div = data->fan_div[nr]; /* No change */
		dev_info(dev, "fan%u low limit and alarm disabled\n", nr + 1);
	} else if ((reg = 1350000U / val) >= 128 * 255) {
		/*
		 * Speed below this value cannot possibly be represented,
		 * even with the highest divider (128)
		 */
		data->fan_min[nr] = 254;
		new_div = 7; /* 128 == (1 << 7) */
		dev_warn(dev,
			 "fan%u low limit %lu below minimum %u, set to minimum\n",
			 nr + 1, val, data->fan_from_reg_min(254, 7));
	} else if (!reg) {
		/*
		 * Speed above this value cannot possibly be represented,
		 * even with the lowest divider (1)
		 */
		data->fan_min[nr] = 1;
		new_div = 0; /* 1 == (1 << 0) */
		dev_warn(dev,
			 "fan%u low limit %lu above maximum %u, set to maximum\n",
			 nr + 1, val, data->fan_from_reg_min(1, 0));
	} else {
		/*
		 * Automatically pick the best divider, i.e. the one such
		 * that the min limit will correspond to a register value
		 * in the 96..192 range
		 */
		new_div = 0;
		while (reg > 192 && new_div < 7) {
			reg >>= 1;
			new_div++;
		}
		data->fan_min[nr] = reg;
	}

	/*
	 * Write both the fan clock divider (if it changed) and the new
	 * fan min (unconditionally)
	 */
	if (new_div != data->fan_div[nr]) {
		dev_dbg(dev, "fan%u clock divider changed from %u to %u\n",
			nr + 1, div_from_reg(data->fan_div[nr]),
			div_from_reg(new_div));
		data->fan_div[nr] = new_div;
		w83627ehf_write_fan_div_common(dev, data, nr);
		/* Give the chip time to sample a new speed value */
		data->last_updated = jiffies;
	}
done:
	w83627ehf_write_value(data, data->REG_FAN_MIN[nr], data->fan_min[nr]);
	mutex_unlock(&data->update_lock);
}

#define show_tol_temp(reg) \
static ssize_t show_##reg(struct device *dev, struct device_attribute *attr, \
				char *buf) \
{ \
	struct w83627ehf_data *data = w83627ehf_update_device(dev); \
	struct sensor_device_attribute *sensor_attr = \
		to_sensor_dev_attr(attr); \
	int nr = sensor_attr->index; \
	return sprintf(buf, "%d\n", data->reg[nr] * 1000); \
}

show_tol_temp(tolerance)
show_tol_temp(target_temp)

static ssize_t
store_target_temp(struct device *dev, struct device_attribute *attr,
		  const char *buf, size_t count)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute *sensor_attr = to_sensor_dev_attr(attr);
	int nr = sensor_attr->index;
	long val;
	int err;

	err = kstrtol(buf, 10, &val);
	if (err < 0)
		return err;

	val = clamp_val(DIV_ROUND_CLOSEST(val, 1000), 0, 127);

	mutex_lock(&data->update_lock);
	data->target_temp[nr] = val;
	w83627ehf_write_value(data, data->REG_TARGET[nr], val);
	mutex_unlock(&data->update_lock);
	return count;
}

static ssize_t
store_tolerance(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);
	struct w83627ehf_sio_data *sio_data = data->sio_data;
	struct sensor_device_attribute *sensor_attr = to_sensor_dev_attr(attr);
	int nr = sensor_attr->index;
	u16 reg;
	long val;
	int err;

	err = kstrtol(buf, 10, &val);
	if (err < 0)
		return err;

	/* Limit the temp to 0C - 15C */
	val = clamp_val(DIV_ROUND_CLOSEST(val, 1000), 0, 15);

	mutex_lock(&data->update_lock);
	if (sio_data->kind == nct6775 || sio_data->kind == nct6776) {
		/* Limit tolerance further for NCT6776F */
		if (sio_data->kind == nct6776 && val > 7)
			val = 7;
		reg = w83627ehf_read_value(data, NCT6775_REG_FAN_MODE[nr]);
		reg = (reg & 0xf0) | val;
		w83627ehf_write_value(data, NCT6775_REG_FAN_MODE[nr], reg);
	} else {
		reg = w83627ehf_read_value(data, W83627EHF_REG_TOLERANCE[nr]);
		if (nr == 1)
			reg = (reg & 0x0f) | (val << 4);
		else
			reg = (reg & 0xf0) | val;
		w83627ehf_write_value(data, W83627EHF_REG_TOLERANCE[nr], reg);
	}
	data->tolerance[nr] = val;
	mutex_unlock(&data->update_lock);
	return count;
}

static struct sensor_device_attribute sda_target_temp[] = {
	SENSOR_ATTR(pwm1_target, 0644, show_target_temp, store_target_temp, 0),
	SENSOR_ATTR(pwm2_target, 0644, show_target_temp, store_target_temp, 1),
	SENSOR_ATTR(pwm3_target, 0644, show_target_temp, store_target_temp, 2),
	SENSOR_ATTR(pwm4_target, 0644, show_target_temp, store_target_temp, 3),
};

static struct sensor_device_attribute sda_tolerance[] = {
	SENSOR_ATTR(pwm1_tolerance, 0644, show_tolerance, store_tolerance, 0),
	SENSOR_ATTR(pwm2_tolerance, 0644, show_tolerance, store_tolerance, 1),
	SENSOR_ATTR(pwm3_tolerance, 0644, show_tolerance, store_tolerance, 2),
	SENSOR_ATTR(pwm4_tolerance, 0644, show_tolerance, store_tolerance, 3),
};

/* Smart Fan registers */

#define fan_functions(reg, REG) \
static ssize_t show_##reg(struct device *dev, struct device_attribute *attr, \
		       char *buf) \
{ \
	struct w83627ehf_data *data = w83627ehf_update_device(dev); \
	struct sensor_device_attribute *sensor_attr = \
		to_sensor_dev_attr(attr); \
	int nr = sensor_attr->index; \
	return sprintf(buf, "%d\n", data->reg[nr]); \
} \
static ssize_t \
store_##reg(struct device *dev, struct device_attribute *attr, \
			    const char *buf, size_t count) \
{ \
	struct w83627ehf_data *data = dev_get_drvdata(dev); \
	struct sensor_device_attribute *sensor_attr = \
		to_sensor_dev_attr(attr); \
	int nr = sensor_attr->index; \
	unsigned long val; \
	int err; \
	err = kstrtoul(buf, 10, &val); \
	if (err < 0) \
		return err; \
	val = clamp_val(val, 1, 255); \
	mutex_lock(&data->update_lock); \
	data->reg[nr] = val; \
	w83627ehf_write_value(data, data->REG_##REG[nr], val); \
	mutex_unlock(&data->update_lock); \
	return count; \
}

fan_functions(fan_start_output, FAN_START_OUTPUT)
fan_functions(fan_stop_output, FAN_STOP_OUTPUT)
fan_functions(fan_max_output, FAN_MAX_OUTPUT)
fan_functions(fan_step_output, FAN_STEP_OUTPUT)

#define fan_time_functions(reg, REG) \
static ssize_t show_##reg(struct device *dev, struct device_attribute *attr, \
				char *buf) \
{ \
	struct w83627ehf_data *data = w83627ehf_update_device(dev); \
	struct sensor_device_attribute *sensor_attr = \
		to_sensor_dev_attr(attr); \
	int nr = sensor_attr->index; \
	return sprintf(buf, "%d\n", \
			step_time_from_reg(data->reg[nr], \
					   data->pwm_mode[nr])); \
} \
\
static ssize_t \
store_##reg(struct device *dev, struct device_attribute *attr, \
			const char *buf, size_t count) \
{ \
	struct w83627ehf_data *data = dev_get_drvdata(dev); \
	struct sensor_device_attribute *sensor_attr = \
		to_sensor_dev_attr(attr); \
	int nr = sensor_attr->index; \
	unsigned long val; \
	int err; \
	err = kstrtoul(buf, 10, &val); \
	if (err < 0) \
		return err; \
	val = step_time_to_reg(val, data->pwm_mode[nr]); \
	mutex_lock(&data->update_lock); \
	data->reg[nr] = val; \
	w83627ehf_write_value(data, data->REG_##REG[nr], val); \
	mutex_unlock(&data->update_lock); \
	return count; \
} \

fan_time_functions(fan_stop_time, FAN_STOP_TIME)

static struct sensor_device_attribute sda_sf3_arrays_fan4[] = {
	SENSOR_ATTR(pwm4_stop_time, 0644, show_fan_stop_time,
		    store_fan_stop_time, 3),
	SENSOR_ATTR(pwm4_start_output, 0644, show_fan_start_output,
		    store_fan_start_output, 3),
	SENSOR_ATTR(pwm4_stop_output, 0644, show_fan_stop_output,
		    store_fan_stop_output, 3),
	SENSOR_ATTR(pwm4_max_output, 0644, show_fan_max_output,
		    store_fan_max_output, 3),
	SENSOR_ATTR(pwm4_step_output, 0644, show_fan_step_output,
		    store_fan_step_output, 3),
};

static struct sensor_device_attribute sda_sf3_arrays_fan3[] = {
	SENSOR_ATTR(pwm3_stop_time, 0644, show_fan_stop_time,
		    store_fan_stop_time, 2),
	SENSOR_ATTR(pwm3_start_output, 0644, show_fan_start_output,
		    store_fan_start_output, 2),
	SENSOR_ATTR(pwm3_stop_output, 0644, show_fan_stop_output,
		    store_fan_stop_output, 2),
};

static SENSOR_DEVICE_ATTR(pwm1_stop_time, 0644, show_fan_stop_time,
			  store_fan_stop_time, 0);
static SENSOR_DEVICE_ATTR(pwm2_stop_time, 0644, show_fan_stop_time,
			  store_fan_stop_time, 1);
static SENSOR_DEVICE_ATTR(pwm1_start_output, 0644, show_fan_start_output,
			  store_fan_start_output, 0);
static SENSOR_DEVICE_ATTR(pwm2_start_output, 0644, show_fan_start_output,
			  store_fan_start_output, 1);
static SENSOR_DEVICE_ATTR(pwm1_stop_output, 0644, show_fan_stop_output,
			  store_fan_stop_output, 0);
static SENSOR_DEVICE_ATTR(pwm2_stop_output, 0644, show_fan_stop_output,
			  store_fan_stop_output, 1);

/*
 * pwm1 and pwm3 don't support max and step settings on all chips.
 * Need to check support while generating/removing attribute files.
 */
static struct sensor_device_attribute sda_sf3_max_step_arrays[] = {
	SENSOR_ATTR(pwm1_max_output, 0644, show_fan_max_output,
		    store_fan_max_output, 0),
	SENSOR_ATTR(pwm1_step_output, 0644, show_fan_step_output,
		    store_fan_step_output, 0),
	SENSOR_ATTR(pwm2_max_output, 0644, show_fan_max_output,
		    store_fan_max_output, 1),
	SENSOR_ATTR(pwm2_step_output, 0644, show_fan_step_output,
		    store_fan_step_output, 1),
	SENSOR_ATTR(pwm3_max_output, 0644, show_fan_max_output,
		    store_fan_max_output, 2),
	SENSOR_ATTR(pwm3_step_output, 0644, show_fan_step_output,
		    store_fan_step_output, 2),
};

static ssize_t
cpu0_vid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", vid_from_reg(data->vid, data->vrm));
}
static DEVICE_ATTR_RO(cpu0_vid);

/* Case open detection */

static ssize_t
show_caseopen(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct w83627ehf_data *data = w83627ehf_update_device(dev);

	return sprintf(buf, "%d\n",
		!!(data->caseopen & to_sensor_dev_attr_2(attr)->index));
}

static ssize_t
clear_caseopen(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);
	unsigned long val;
	u16 reg, mask;

	if (kstrtoul(buf, 10, &val) || val != 0)
		return -EINVAL;

	mask = to_sensor_dev_attr_2(attr)->nr;

	mutex_lock(&data->update_lock);
	reg = w83627ehf_read_value(data, W83627EHF_REG_CASEOPEN_CLR);
	w83627ehf_write_value(data, W83627EHF_REG_CASEOPEN_CLR, reg | mask);
	w83627ehf_write_value(data, W83627EHF_REG_CASEOPEN_CLR, reg & ~mask);
	data->valid = 0;	/* Force cache refresh */
	mutex_unlock(&data->update_lock);

	return count;
}

static SENSOR_DEVICE_ATTR_2(intrusion0_alarm, 0644, show_caseopen,
			    clear_caseopen, 0x80, 0x10);
static SENSOR_DEVICE_ATTR_2(intrusion1_alarm, 0644, show_caseopen,
			    clear_caseopen, 0x40, 0x40);

static struct attribute *static_sensor_attrs[] = {
	&sensor_dev_attr_pwm1_stop_time.dev_attr.attr,
	&sensor_dev_attr_pwm2_stop_time.dev_attr.attr,
	&sensor_dev_attr_pwm1_start_output.dev_attr.attr,
	&sensor_dev_attr_pwm2_start_output.dev_attr.attr,
	&sensor_dev_attr_pwm1_stop_output.dev_attr.attr,
	&sensor_dev_attr_pwm2_stop_output.dev_attr.attr,
	&sensor_dev_attr_intrusion0_alarm.dev_attr.attr,
	NULL,
};

static struct attribute *other_sensor_attrs[] = {
	&dev_attr_cpu0_vid.attr,
	&sensor_dev_attr_intrusion1_alarm.dev_attr.attr,
	NULL,
};

static struct attribute *sda_sf3_max_step_arrays_attrs[] = {
	&sda_sf3_max_step_arrays[0].dev_attr.attr,
	&sda_sf3_max_step_arrays[1].dev_attr.attr,
	&sda_sf3_max_step_arrays[2].dev_attr.attr,
	&sda_sf3_max_step_arrays[3].dev_attr.attr,
	&sda_sf3_max_step_arrays[4].dev_attr.attr,
	&sda_sf3_max_step_arrays[5].dev_attr.attr,
	NULL,
};

static struct attribute *sda_sf3_arrays_fan3_attrs[] = {
	&sda_sf3_arrays_fan3[0].dev_attr.attr,
	&sda_sf3_arrays_fan3[1].dev_attr.attr,
	&sda_sf3_arrays_fan3[2].dev_attr.attr,
	NULL,
};

static struct attribute *sda_sf3_arrays_fan4_attrs[] = {
	&sda_sf3_arrays_fan4[0].dev_attr.attr,
	&sda_sf3_arrays_fan4[1].dev_attr.attr,
	&sda_sf3_arrays_fan4[2].dev_attr.attr,
	&sda_sf3_arrays_fan4[3].dev_attr.attr,
	&sda_sf3_arrays_fan4[4].dev_attr.attr,
	NULL,
};

static struct attribute *sda_target_temp_attrs[] = {
	&sda_target_temp[0].dev_attr.attr,
	&sda_target_temp[1].dev_attr.attr,
	&sda_target_temp[2].dev_attr.attr,
	&sda_target_temp[3].dev_attr.attr,
	NULL,
};

static struct attribute *sda_tolerance_attrs[] = {
	&sda_tolerance[0].dev_attr.attr,
	&sda_tolerance[1].dev_attr.attr,
	&sda_tolerance[2].dev_attr.attr,
	&sda_tolerance[3].dev_attr.attr,
	NULL,
};

static umode_t other_sensor_is_visible(struct kobject *kobj,
				       struct attribute *attr, int idx)
{
	struct device *dev = kobj_to_dev(kobj);
	struct w83627ehf_data *data = dev_get_drvdata(dev);

	if (idx == 0) { /*dev_attr_cpu0_vid*/
		if (data->sio_data->kind == w83627uhg)
			return 0;
		else
			return 0444;
	}
	if (idx == 1) { /*intrusion1*/
		if (data->sio_data->kind == nct6776)
			return 0644;
	}

	return 0;
}

static umode_t sda_sf3_max_step_arrays_is_visible(struct kobject *kobj,
						  struct attribute *attr,
						  int idx)
{
	struct device *dev = kobj_to_dev(kobj);
	struct w83627ehf_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute *sattr = &sda_sf3_max_step_arrays[idx];

	if (data->REG_FAN_STEP_OUTPUT &&
	    data->REG_FAN_STEP_OUTPUT[sattr->index] != 0xff)
		return 0644;
	else
		return 0;
}

static umode_t sda_sf3_arrays_fan3_attrs_is_visible(struct kobject *kobj,
						    struct attribute *attr,
						    int idx)
{
	struct device *dev = kobj_to_dev(kobj);
	struct w83627ehf_data *data = dev_get_drvdata(dev);

	if ((data->has_fan & (1 << 2)) && data->pwm_num >= 3)
		return 0644;

	return 0;
}

static umode_t sda_sf3_arrays_fan4_is_visible(struct kobject *kobj,
					      struct attribute *attr, int idx)
{
	struct device *dev = kobj_to_dev(kobj);
	struct w83627ehf_data *data = dev_get_drvdata(dev);

	if ((data->has_fan & (1 << 3)) && data->pwm_num >= 4)
		return 0644;

	return 0;
}

static umode_t sda_tolerance_is_visible(struct kobject *kobj,
					struct attribute *attr, int idx)
{
	struct device *dev = kobj_to_dev(kobj);
	struct w83627ehf_data *data = dev_get_drvdata(dev);

	if ((data->has_fan & (1 << idx)) && (idx < data->pwm_num))
		return 0644;
	return 0;
}

static umode_t sda_target_temp_is_visible(struct kobject *kobj,
					  struct attribute *attr, int idx)
{
	struct device *dev = kobj_to_dev(kobj);
	struct w83627ehf_data *data = dev_get_drvdata(dev);

	if ((data->has_fan & (1 << idx)) && (idx < data->pwm_num))
		return 0644;
	return 0;
}

static const struct attribute_group static_sensor_group = {
	.attrs = static_sensor_attrs,
};

static const struct attribute_group other_sensor_group = {
	.attrs = other_sensor_attrs,
	.is_visible = other_sensor_is_visible,
};

static const struct attribute_group sda_sf3_max_step_arrays_group = {
	.attrs = sda_sf3_max_step_arrays_attrs,
	.is_visible = sda_sf3_max_step_arrays_is_visible,
};

static const struct attribute_group sda_sf3_arrays_fan3_group = {
	.attrs = sda_sf3_arrays_fan3_attrs,
	.is_visible = sda_sf3_arrays_fan3_attrs_is_visible,
};

static const struct attribute_group sda_sf3_arrays_fan4_group = {
	.attrs = sda_sf3_arrays_fan4_attrs,
	.is_visible = sda_sf3_arrays_fan4_is_visible,
};

static const struct attribute_group sda_tolerance_group = {
	.attrs = sda_tolerance_attrs,
	.is_visible = sda_tolerance_is_visible,
};

static const struct attribute_group sda_target_temp_group = {
	.attrs = sda_target_temp_attrs,
	.is_visible = sda_target_temp_is_visible,
};

static const struct attribute_group *sensor_groups[] = {
	&static_sensor_group,
	&other_sensor_group,
	&sda_sf3_max_step_arrays_group,
	&sda_sf3_arrays_fan3_group,
	&sda_sf3_arrays_fan4_group,
	&sda_tolerance_group,
	&sda_target_temp_group,
	NULL,
};

/*
 * Driver and device management
 */

/* Get the monitoring functions started */
static inline void w83627ehf_init_device(struct w83627ehf_data *data,
						   enum kinds kind)
{
	int i;
	u8 tmp, diode;

	/* Start monitoring is needed */
	tmp = w83627ehf_read_value(data, W83627EHF_REG_CONFIG);
	if (!(tmp & 0x01))
		w83627ehf_write_value(data, W83627EHF_REG_CONFIG, tmp | 0x01);

	/* Enable temperature sensors if needed */
	for (i = 0; i < NUM_REG_TEMP; i++) {
		if (!(data->have_temp & (1 << i)))
			continue;
		if (!data->reg_temp_config[i])
			continue;
		tmp = w83627ehf_read_value(data, data->reg_temp_config[i]);
		if (tmp & 0x01)
			w83627ehf_write_value(data,
					      data->reg_temp_config[i],
					      tmp & 0xfe);
	}

	/* Enable VBAT monitoring if needed */
	tmp = w83627ehf_read_value(data, W83627EHF_REG_VBAT);
	if (!(tmp & 0x01))
		w83627ehf_write_value(data, W83627EHF_REG_VBAT, tmp | 0x01);

	/* Get thermal sensor types */
	switch (kind) {
	case w83627ehf:
		diode = w83627ehf_read_value(data, W83627EHF_REG_DIODE);
		break;
	case w83627uhg:
		diode = 0x00;
		break;
	default:
		diode = 0x70;
	}
	for (i = 0; i < 3; i++) {
		const char *label = NULL;

		if (data->temp_label)
			label = data->temp_label[data->temp_src[i]];

		/* Digital source overrides analog type */
		if (label && strncmp(label, "PECI", 4) == 0)
			data->temp_type[i] = 6;
		else if (label && strncmp(label, "AMD", 3) == 0)
			data->temp_type[i] = 5;
		else if ((tmp & (0x02 << i)))
			data->temp_type[i] = (diode & (0x10 << i)) ? 1 : 3;
		else
			data->temp_type[i] = 4; /* thermistor */
	}
}

static void w82627ehf_swap_tempreg(struct w83627ehf_data *data, int r1, int r2)
{
	swap(data->temp_src[r1], data->temp_src[r2]);
	swap(data->reg_temp[r1], data->reg_temp[r2]);
	swap(data->reg_temp_over[r1], data->reg_temp_over[r2]);
	swap(data->reg_temp_hyst[r1], data->reg_temp_hyst[r2]);
	swap(data->reg_temp_config[r1], data->reg_temp_config[r2]);
}

static void w83627ehf_set_temp_reg_ehf(struct w83627ehf_data *data, int n_temp)
{
	int i;

	for (i = 0; i < n_temp; i++) {
		data->reg_temp[i] = W83627EHF_REG_TEMP[i];
		data->reg_temp_over[i] = W83627EHF_REG_TEMP_OVER[i];
		data->reg_temp_hyst[i] = W83627EHF_REG_TEMP_HYST[i];
		data->reg_temp_config[i] = W83627EHF_REG_TEMP_CONFIG[i];
	}
}

static void
w83627ehf_check_fan_inputs(const struct w83627ehf_sio_data *sio_data,
			   struct w83627ehf_data *data)
{
	int fan3pin, fan4pin, fan4min, fan5pin, regval;

	/* The W83627UHG is simple, only two fan inputs, no config */
	if (sio_data->kind == w83627uhg) {
		data->has_fan = 0x03; /* fan1 and fan2 */
		data->has_fan_min = 0x03;
		return;
	}

	/* fan4 and fan5 share some pins with the GPIO and serial flash */
	if (sio_data->kind == nct6775) {
		/* On NCT6775, fan4 shares pins with the fdc interface */
		fan3pin = 1;
		fan4pin = !(superio_inb(sio_data->sioreg, 0x2A) & 0x80);
		fan4min = 0;
		fan5pin = 0;
	} else if (sio_data->kind == nct6776) {
		bool gpok = superio_inb(sio_data->sioreg, 0x27) & 0x80;

		superio_select(sio_data->sioreg, W83627EHF_LD_HWM);
		regval = superio_inb(sio_data->sioreg, SIO_REG_ENABLE);

		if (regval & 0x80)
			fan3pin = gpok;
		else
			fan3pin = !(superio_inb(sio_data->sioreg, 0x24) & 0x40);

		if (regval & 0x40)
			fan4pin = gpok;
		else
			fan4pin = !!(superio_inb(sio_data->sioreg, 0x1C) & 0x01);

		if (regval & 0x20)
			fan5pin = gpok;
		else
			fan5pin = !!(superio_inb(sio_data->sioreg, 0x1C) & 0x02);

		fan4min = fan4pin;
	} else if (sio_data->kind == w83667hg || sio_data->kind == w83667hg_b) {
		fan3pin = 1;
		fan4pin = superio_inb(sio_data->sioreg, 0x27) & 0x40;
		fan5pin = superio_inb(sio_data->sioreg, 0x27) & 0x20;
		fan4min = fan4pin;
	} else {
		fan3pin = 1;
		fan4pin = !(superio_inb(sio_data->sioreg, 0x29) & 0x06);
		fan5pin = !(superio_inb(sio_data->sioreg, 0x24) & 0x02);
		fan4min = fan4pin;
	}

	data->has_fan = data->has_fan_min = 0x03; /* fan1 and fan2 */
	data->has_fan |= (fan3pin << 2);
	data->has_fan_min |= (fan3pin << 2);

	if (sio_data->kind == nct6775 || sio_data->kind == nct6776) {
		/*
		 * NCT6775F and NCT6776F don't have the W83627EHF_REG_FANDIV1
		 * register
		 */
		data->has_fan |= (fan4pin << 3) | (fan5pin << 4);
		data->has_fan_min |= (fan4min << 3) | (fan5pin << 4);
	} else {
		/*
		 * It looks like fan4 and fan5 pins can be alternatively used
		 * as fan on/off switches, but fan5 control is write only :/
		 * We assume that if the serial interface is disabled, designers
		 * connected fan5 as input unless they are emitting log 1, which
		 * is not the default.
		 */
		regval = w83627ehf_read_value(data, W83627EHF_REG_FANDIV1);
		if ((regval & (1 << 2)) && fan4pin) {
			data->has_fan |= (1 << 3);
			data->has_fan_min |= (1 << 3);
		}
		if (!(regval & (1 << 1)) && fan5pin) {
			data->has_fan |= (1 << 4);
			data->has_fan_min |= (1 << 4);
		}
	}
}

static int w3627ehf_read_temp(struct device *dev, u32 attr, int channel,
			      long *val)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_temp_input:
		*val = LM75_TEMP_FROM_REG(data->temp[channel]);
		return 0;
	case hwmon_temp_max:
		*val = LM75_TEMP_FROM_REG(data->temp_max[channel]);
		return 0;
	case hwmon_temp_max_hyst:
		*val = LM75_TEMP_FROM_REG(data->temp_max_hyst[channel]);
		return 0;
	case hwmon_temp_type:
		*val = data->temp_type[channel];
		return 0;
	case hwmon_temp_offset:
		*val = data->temp_offset[channel] * 1000;
		return 0;
	case hwmon_temp_alarm:
		switch (channel) {
		case 0:
			channel = 4;
			break;
		case 1:
			channel = 5;
			break;
		case 2:
			channel = 13;
			break;
		default:
			return -EINVAL;
		}
		*val = ((data->alarms >> channel) & 0x01);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int w3627ehf_write_temp(struct device *dev, u32 attr, int channel,
			       long val)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_temp_offset:
		val = clamp_val(DIV_ROUND_CLOSEST(val, 1000), -128, 127);
		mutex_lock(&data->update_lock);
		data->temp_offset[channel] = val;
		w83627ehf_write_value(data, W83627EHF_REG_TEMP_OFFSET[channel],
				      val);
		mutex_unlock(&data->update_lock);
		break;
	case hwmon_temp_max:
		mutex_lock(&data->update_lock);
		data->temp_max[channel] = LM75_TEMP_TO_REG(val);
		w83627ehf_write_temp(data, data->reg_temp_over[channel],
				     data->temp_max[channel]);
		mutex_unlock(&data->update_lock);
		break;
	case hwmon_temp_max_hyst:
		mutex_lock(&data->update_lock);
		data->temp_max_hyst[channel] = LM75_TEMP_TO_REG(val);
		w83627ehf_write_temp(data, data->reg_temp_hyst[channel],
				     data->temp_max_hyst[channel]);
		mutex_unlock(&data->update_lock);
		break;

	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static umode_t w3627ehf_temp_is_visible(const void *_data, u32 attr,
					int channel)
{
	const struct w83627ehf_data *data = _data;

	if (!(data->have_temp & (1 << channel)))
		return 0;

	switch (attr) {
	case hwmon_temp_label:
		if (data->temp_label)
			return 0444;
		return 0;
	case hwmon_temp_input:
		return 0444;
	case hwmon_temp_max:
		if (channel == 2 && data->temp3_val_only)
			return 0;
		else if (data->reg_temp_over[channel])
			return 0644;
		else
			return 0;
	case hwmon_temp_max_hyst:
		if (channel == 2 && data->temp3_val_only)
			return 0;
		else if (data->reg_temp_hyst[channel])
			return 0644;
		else
			return 0;
	case hwmon_temp_alarm:
	case hwmon_temp_type:
		return 0444;
	case hwmon_temp_offset:
		if (data->have_temp_offset & (1 << channel))
			return 0644;
		return 0;
	}
	return 0;
}

static int w3627ehf_write_fan(struct device *dev, u32 attr, int channel,
			      long val)
{
	switch (attr) {
	case hwmon_fan_min:
		store_fan_min(dev, channel, val);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int w3627ehf_read_fan(struct device *dev, u32 attr, int channel,
			     long *val)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_fan_alarm:
		//the fan sensors have different offsets
		switch (channel) {
		case 0:
			channel = 6;
			break;
		case 1:
			channel = 7;
			break;
		case 2:
			channel = 11;
			break;
		case 3:
			channel = 10;
			break;
		case 4:
			channel = 23;
			break;
		default:
			return -EINVAL;
		}
		*val = ((data->alarms >> channel) & 0x01);
		return 0;
	case hwmon_fan_input:
		*val = data->rpm[channel];
		return 0;
	case hwmon_fan_min:
		*val = data->fan_from_reg_min(data->fan_min[channel],
					      data->fan_div[channel]);
		return 0;
	case hwmon_fan_div:
		*val = div_from_reg(data->fan_div[channel]);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t w3627ehf_fan_is_visible(const void *_data, u32 attr, int channel)
{
	const struct w83627ehf_data *data = _data;

	if (!(data->has_fan & (1 << channel)))
		return 0;

	switch (attr) {
	case hwmon_fan_alarm:
	case hwmon_fan_input:
		return 0444;
	case hwmon_fan_div:
		if (data->sio_data->kind != nct6776)
			return 0444;
		else
			return 0;
	case hwmon_fan_min:
		return 0644;
	}
	return 0;
}

static int w3627ehf_write_pwm(struct device *dev, u32 attr, int channel,
			      long val)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);
	struct w83627ehf_sio_data *sio_data = data->sio_data;
	u16 reg;

	switch (attr) {
	case hwmon_pwm_input:
		val = clamp_val(val, 0, 255);
		mutex_lock(&data->update_lock);
		data->pwm[channel] = val;
		w83627ehf_write_value(data, data->REG_PWM[channel], val);
		mutex_unlock(&data->update_lock);
		return 0;
	case hwmon_pwm_mode:
		if (val > 1)
			return -EINVAL;

		/* On NCT67766F, DC mode is only supported for pwm1 */
		if (sio_data->kind == nct6776 && channel && val != 1)
			return -EINVAL;

		mutex_lock(&data->update_lock);
		reg = w83627ehf_read_value(data,
					   W83627EHF_REG_PWM_ENABLE[channel]);
		data->pwm_mode[channel] = val;
		reg &= ~(1 << W83627EHF_PWM_MODE_SHIFT[channel]);
		if (!val)
			reg |= 1 << W83627EHF_PWM_MODE_SHIFT[channel];
		w83627ehf_write_value(data, W83627EHF_REG_PWM_ENABLE[channel],
				      reg);
		mutex_unlock(&data->update_lock);
		return 0;
	case hwmon_pwm_enable:
		if (!val || (val > 4 && val != data->pwm_enable_orig[channel]))
			return -EINVAL;
		/* SmartFan III mode is not supported on NCT6776F */
		if (sio_data->kind == nct6776 && val == 4)
			return -EINVAL;

		mutex_lock(&data->update_lock);
		data->pwm_enable[channel] = val;
		if (sio_data->kind == nct6775 || sio_data->kind == nct6776) {
			reg = w83627ehf_read_value(data,
						   NCT6775_REG_FAN_MODE[channel]);
			reg &= 0x0f;
			reg |= (val - 1) << 4;
			w83627ehf_write_value(data,
					      NCT6775_REG_FAN_MODE[channel],
					      reg);
		} else {
			reg = w83627ehf_read_value(data,
						   W83627EHF_REG_PWM_ENABLE[channel]);
			reg &= ~(0x03 << W83627EHF_PWM_ENABLE_SHIFT[channel]);
			reg |= (val - 1) << W83627EHF_PWM_ENABLE_SHIFT[channel];
			w83627ehf_write_value(data,
					      W83627EHF_REG_PWM_ENABLE[channel],
					      reg);
		}
		mutex_unlock(&data->update_lock);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int w3627ehf_write_input(struct device *dev, u32 attr, int channel,
				long val)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_in_min:
		mutex_lock(&data->update_lock);
		data->in_min[channel] = in_to_reg(val, channel, data->scale_in);
		w83627ehf_write_value(data, W83627EHF_REG_IN_MIN(channel),
				      data->in_min[channel]);
		mutex_unlock(&data->update_lock);
		return 0;
	case hwmon_in_max:
		mutex_lock(&data->update_lock);
		data->in_max[channel] = in_to_reg(val, channel, data->scale_in);
		w83627ehf_write_value(data, W83627EHF_REG_IN_MAX(channel),
				      data->in_max[channel]);
		mutex_unlock(&data->update_lock);
		return 0;
	}
	return -EOPNOTSUPP;
}

static int w3627ehf_read_pwm(struct device *dev, u32 attr, int channel,
			     long *val)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_pwm_input:
		*val = data->pwm[channel];
		return 0;
	case hwmon_pwm_mode:
		*val = data->pwm_mode[channel];
		return 0;
	case hwmon_pwm_enable:
		*val = data->pwm_enable[channel];
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t w3627ehf_pwm_is_visible(const void *_data, u32 attr, int channel)
{
	const struct w83627ehf_data *data = _data;

	switch (attr) {
	case hwmon_pwm_input:
	case hwmon_pwm_mode:
	case hwmon_pwm_enable:
		if (data->has_fan & (1 << channel)) {
			if (channel < data->pwm_num)
				return 0644;
		}
		return 0;
	}
	return 0;
}

static int w3627ehf_read_input(struct device *dev, u32 attr, int channel,
			       long *val)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);

	switch (attr) {
	case hwmon_in_input:
		*val = in_from_reg(data->in[channel], channel, data->scale_in);
		return 0;
	case hwmon_in_min:
		*val = in_from_reg(data->in_min[channel], channel,
				   data->scale_in);
		return 0;
	case hwmon_in_max:
		*val = in_from_reg(data->in_max[channel], channel,
				   data->scale_in);
		return 0;
	case hwmon_in_alarm:
		switch (channel) {
		case 0:
			channel = 0;
			break;
		case 1:
			channel = 1;
			break;
		case 2:
			channel = 2;
			break;
		case 3:
			channel = 3;
			break;
		case 4:
			channel = 8;
			break;
		case 5:
			channel = 21;
			break;
		case 6:
			channel = 20;
			break;
		case 7:
			channel = 16;
			break;
		case 8:
			channel = 17;
			break;
		case 9:
			channel = 19;
			break;
		default:
			return -EINVAL;
		}
		*val = ((data->alarms >> channel) & 0x01);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t w3627ehf_input_is_visible(const void *_data, u32 attr,
					 int channel)
{
	const struct w83627ehf_data *data = _data;

	//skip channel 6 if requested
	if ((channel == 6) && data->in6_skip)
		return 0;

	switch (attr) {
	case hwmon_in_input:
	case hwmon_in_alarm:
		return 0444;
	case hwmon_in_min:
	case hwmon_in_max:
		return 0644;
	default:
		return 0;
	}
}

static int w3627ehf_read_string(struct device *dev,
				enum hwmon_sensor_types type, u32 attr,
				int channel, const char **buf)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);

	w83627ehf_update_device(dev);
	if ((type == hwmon_fan && attr == hwmon_fan_label) ||
	    (type == hwmon_temp && attr == hwmon_temp_label)) {
		*buf = data->temp_label[data->temp_src[channel]];
		return 0;
	}
	return -EOPNOTSUPP;
}

static int w3627ehf_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *val)
{
	w83627ehf_update_device(dev);
	switch (type) {
	case hwmon_fan:
		return w3627ehf_read_fan(dev, attr, channel, val);
	case hwmon_temp:
		return w3627ehf_read_temp(dev, attr, channel, val);
	case hwmon_in:
		return w3627ehf_read_input(dev, attr, channel, val);
	case hwmon_pwm:
		return w3627ehf_read_pwm(dev, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int w3627ehf_write(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long val)
{
	switch (type) {
	case hwmon_fan:
		return w3627ehf_write_fan(dev, attr, channel, val);
	case hwmon_in:
		return w3627ehf_write_input(dev, attr, channel, val);
	case hwmon_pwm:
		return w3627ehf_write_pwm(dev, attr, channel, val);
	case hwmon_temp:
		return w3627ehf_write_temp(dev, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t w3627ehf_is_visible(const void *data,
				   enum hwmon_sensor_types type, u32 attr,
				   int channel)
{
	switch (type) {
	case hwmon_fan:
		return w3627ehf_fan_is_visible(data, attr, channel);
	case hwmon_pwm:
		return w3627ehf_pwm_is_visible(data, attr, channel);
	case hwmon_in:
		return w3627ehf_input_is_visible(data, attr, channel);
	case hwmon_temp:
		return w3627ehf_temp_is_visible(data, attr, channel);
	default:
		return 0;
	}
}

static const u32 w3627ehf_temp_config[] = {
	HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX | HWMON_T_MAX_HYST |
	    HWMON_T_ALARM | HWMON_T_TYPE | HWMON_T_OFFSET,
	HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX | HWMON_T_MAX_HYST |
	    HWMON_T_ALARM | HWMON_T_TYPE | HWMON_T_OFFSET,
	HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX | HWMON_T_MAX_HYST |
	    HWMON_T_ALARM | HWMON_T_TYPE | HWMON_T_OFFSET,
	HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX | HWMON_T_MAX_HYST,
	HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX | HWMON_T_MAX_HYST,
	HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX | HWMON_T_MAX_HYST,
	HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX | HWMON_T_MAX_HYST,
	HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX | HWMON_T_MAX_HYST,
	HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX | HWMON_T_MAX_HYST,
	0
};

static const u32 w3627ehf_fan_config[] = {
	HWMON_F_INPUT | HWMON_F_ALARM | HWMON_F_DIV | HWMON_F_MIN,
	HWMON_F_INPUT | HWMON_F_ALARM | HWMON_F_DIV | HWMON_F_MIN,
	HWMON_F_INPUT | HWMON_F_ALARM | HWMON_F_DIV | HWMON_F_MIN,
	HWMON_F_INPUT | HWMON_F_ALARM | HWMON_F_DIV | HWMON_F_MIN,
	HWMON_F_INPUT | HWMON_F_ALARM | HWMON_F_DIV | HWMON_F_MIN,
	0
};

static const u32 w3627ehf_pwm_config[] = {
	HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE,
	HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE,
	HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE,
	HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_MODE,
	0
};

static const u32 w3627ehf_input_config[] = {
	HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_ALARM,
	HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_ALARM,
	HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_ALARM,
	HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_ALARM,
	HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_ALARM,
	HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_ALARM,
	HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_ALARM,
	HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_ALARM,
	HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX | HWMON_I_ALARM,
	0
};

static const struct hwmon_channel_info w3627ehf_temp = {
	.type = hwmon_temp,
	.config = w3627ehf_temp_config,
};

static const struct hwmon_channel_info w3627ehf_fan = {
	.type = hwmon_fan,
	.config = w3627ehf_fan_config,
};

static const struct hwmon_channel_info w3627ehf_pwm = {
	.type = hwmon_pwm,
	.config = w3627ehf_pwm_config,
};

static const struct hwmon_channel_info w3627ehf_input = {
	.type = hwmon_in,
	.config = w3627ehf_input_config,
};

static const struct hwmon_ops w3627ehf_hwmon_ops = {
	.is_visible = w3627ehf_is_visible,
	.read = w3627ehf_read,
	.read_string = w3627ehf_read_string,
	.write = w3627ehf_write,
};

static const struct hwmon_channel_info *w3627ehf_info[] = {
	&w3627ehf_temp,
	&w3627ehf_fan,
	&w3627ehf_pwm,
	&w3627ehf_input,
	NULL
};

static const struct hwmon_chip_info w83627ehf_chip_info = {
	.ops = &w3627ehf_hwmon_ops,
	.info = w3627ehf_info,
};

static int w83627ehf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct w83627ehf_sio_data *sio_data = dev_get_platdata(dev);
	struct w83627ehf_data *data;
	struct resource *res;
	u8 en_vrm10;
	int i, err = 0;

	res = platform_get_resource(pdev, IORESOURCE_IO, 0);
	if (!request_region(res->start, IOREGION_LENGTH, DRVNAME)) {
		err = -EBUSY;
		dev_err(dev, "Failed to request region 0x%lx-0x%lx\n",
			(unsigned long)res->start,
			(unsigned long)res->start + IOREGION_LENGTH - 1);
		goto exit;
	}

	data = devm_kzalloc(&pdev->dev, sizeof(struct w83627ehf_data),
			    GFP_KERNEL);
	if (!data) {
		err = -ENOMEM;
		goto exit_release;
	}

	data->sio_data = sio_data;
	data->addr = res->start;
	mutex_init(&data->lock);
	mutex_init(&data->update_lock);
	data->name = w83627ehf_device_names[sio_data->kind];
	data->bank = 0xff;		/* Force initial bank selection */
	platform_set_drvdata(pdev, data);

	/* 627EHG and 627EHF have 10 voltage inputs; 627DHG and 667HG have 9 */
	data->in_num = (sio_data->kind == w83627ehf) ? 10 : 9;
	/* 667HG, NCT6775F, and NCT6776F have 3 pwms, and 627UHG has only 2 */
	switch (sio_data->kind) {
	default:
		data->pwm_num = 4;
		break;
	case w83667hg:
	case w83667hg_b:
	case nct6775:
	case nct6776:
		data->pwm_num = 3;
		break;
	case w83627uhg:
		data->pwm_num = 2;
		break;
	}

	/* Default to 3 temperature inputs, code below will adjust as needed */
	data->have_temp = 0x07;

	/* Deal with temperature register setup first. */
	if (sio_data->kind == nct6775 || sio_data->kind == nct6776) {
		int mask = 0;

		/*
		 * Display temperature sensor output only if it monitors
		 * a source other than one already reported. Always display
		 * first three temperature registers, though.
		 */
		for (i = 0; i < NUM_REG_TEMP; i++) {
			u8 src;

			data->reg_temp[i] = NCT6775_REG_TEMP[i];
			data->reg_temp_over[i] = NCT6775_REG_TEMP_OVER[i];
			data->reg_temp_hyst[i] = NCT6775_REG_TEMP_HYST[i];
			data->reg_temp_config[i] = NCT6775_REG_TEMP_CONFIG[i];

			src = w83627ehf_read_value(data,
						   NCT6775_REG_TEMP_SOURCE[i]);
			src &= 0x1f;
			if (src && !(mask & (1 << src))) {
				data->have_temp |= 1 << i;
				mask |= 1 << src;
			}

			data->temp_src[i] = src;

			/*
			 * Now do some register swapping if index 0..2 don't
			 * point to SYSTIN(1), CPUIN(2), and AUXIN(3).
			 * Idea is to have the first three attributes
			 * report SYSTIN, CPUIN, and AUXIN if possible
			 * without overriding the basic system configuration.
			 */
			if (i > 0 && data->temp_src[0] != 1
			    && data->temp_src[i] == 1)
				w82627ehf_swap_tempreg(data, 0, i);
			if (i > 1 && data->temp_src[1] != 2
			    && data->temp_src[i] == 2)
				w82627ehf_swap_tempreg(data, 1, i);
			if (i > 2 && data->temp_src[2] != 3
			    && data->temp_src[i] == 3)
				w82627ehf_swap_tempreg(data, 2, i);
		}
		if (sio_data->kind == nct6776) {
			/*
			 * On NCT6776, AUXTIN and VIN3 pins are shared.
			 * Only way to detect it is to check if AUXTIN is used
			 * as a temperature source, and if that source is
			 * enabled.
			 *
			 * If that is the case, disable in6, which reports VIN3.
			 * Otherwise disable temp3.
			 */
			if (data->temp_src[2] == 3) {
				u8 reg;

				if (data->reg_temp_config[2])
					reg = w83627ehf_read_value(data,
						data->reg_temp_config[2]);
				else
					reg = 0; /* Assume AUXTIN is used */

				if (reg & 0x01)
					data->have_temp &= ~(1 << 2);
				else
					data->in6_skip = 1;
			}
			data->temp_label = nct6776_temp_label;
		} else {
			data->temp_label = nct6775_temp_label;
		}
		data->have_temp_offset = data->have_temp & 0x07;
		for (i = 0; i < 3; i++) {
			if (data->temp_src[i] > 3)
				data->have_temp_offset &= ~(1 << i);
		}
	} else if (sio_data->kind == w83667hg_b) {
		u8 reg;

		w83627ehf_set_temp_reg_ehf(data, 4);

		/*
		 * Temperature sources are selected with bank 0, registers 0x49
		 * and 0x4a.
		 */
		reg = w83627ehf_read_value(data, 0x4a);
		data->temp_src[0] = reg >> 5;
		reg = w83627ehf_read_value(data, 0x49);
		data->temp_src[1] = reg & 0x07;
		data->temp_src[2] = (reg >> 4) & 0x07;

		/*
		 * W83667HG-B has another temperature register at 0x7e.
		 * The temperature source is selected with register 0x7d.
		 * Support it if the source differs from already reported
		 * sources.
		 */
		reg = w83627ehf_read_value(data, 0x7d);
		reg &= 0x07;
		if (reg != data->temp_src[0] && reg != data->temp_src[1]
		    && reg != data->temp_src[2]) {
			data->temp_src[3] = reg;
			data->have_temp |= 1 << 3;
		}

		/*
		 * Chip supports either AUXTIN or VIN3. Try to find out which
		 * one.
		 */
		reg = w83627ehf_read_value(data, W83627EHF_REG_TEMP_CONFIG[2]);
		if (data->temp_src[2] == 2 && (reg & 0x01))
			data->have_temp &= ~(1 << 2);

		if ((data->temp_src[2] == 2 && (data->have_temp & (1 << 2)))
		    || (data->temp_src[3] == 2 && (data->have_temp & (1 << 3))))
			data->in6_skip = 1;

		data->temp_label = w83667hg_b_temp_label;
		data->have_temp_offset = data->have_temp & 0x07;
		for (i = 0; i < 3; i++) {
			if (data->temp_src[i] > 2)
				data->have_temp_offset &= ~(1 << i);
		}
	} else if (sio_data->kind == w83627uhg) {
		u8 reg;

		w83627ehf_set_temp_reg_ehf(data, 3);

		/*
		 * Temperature sources for temp2 and temp3 are selected with
		 * bank 0, registers 0x49 and 0x4a.
		 */
		data->temp_src[0] = 0;	/* SYSTIN */
		reg = w83627ehf_read_value(data, 0x49) & 0x07;
		/* Adjust to have the same mapping as other source registers */
		if (reg == 0)
			data->temp_src[1] = 1;
		else if (reg >= 2 && reg <= 5)
			data->temp_src[1] = reg + 2;
		else	/* should never happen */
			data->have_temp &= ~(1 << 1);
		reg = w83627ehf_read_value(data, 0x4a);
		data->temp_src[2] = reg >> 5;

		/*
		 * Skip temp3 if source is invalid or the same as temp1
		 * or temp2.
		 */
		if (data->temp_src[2] == 2 || data->temp_src[2] == 3 ||
		    data->temp_src[2] == data->temp_src[0] ||
		    ((data->have_temp & (1 << 1)) &&
		     data->temp_src[2] == data->temp_src[1]))
			data->have_temp &= ~(1 << 2);
		else
			data->temp3_val_only = 1;	/* No limit regs */

		data->in6_skip = 1;			/* No VIN3 */

		data->temp_label = w83667hg_b_temp_label;
		data->have_temp_offset = data->have_temp & 0x03;
		for (i = 0; i < 3; i++) {
			if (data->temp_src[i] > 1)
				data->have_temp_offset &= ~(1 << i);
		}
	} else {
		w83627ehf_set_temp_reg_ehf(data, 3);

		/* Temperature sources are fixed */

		if (sio_data->kind == w83667hg) {
			u8 reg;

			/*
			 * Chip supports either AUXTIN or VIN3. Try to find
			 * out which one.
			 */
			reg = w83627ehf_read_value(data,
						W83627EHF_REG_TEMP_CONFIG[2]);
			if (reg & 0x01)
				data->have_temp &= ~(1 << 2);
			else
				data->in6_skip = 1;
		}
		data->have_temp_offset = data->have_temp & 0x07;
	}

	if (sio_data->kind == nct6775) {
		data->has_fan_div = true;
		data->fan_from_reg = fan_from_reg16;
		data->fan_from_reg_min = fan_from_reg8;
		data->REG_PWM = NCT6775_REG_PWM;
		data->REG_TARGET = NCT6775_REG_TARGET;
		data->REG_FAN = NCT6775_REG_FAN;
		data->REG_FAN_MIN = W83627EHF_REG_FAN_MIN;
		data->REG_FAN_START_OUTPUT = NCT6775_REG_FAN_START_OUTPUT;
		data->REG_FAN_STOP_OUTPUT = NCT6775_REG_FAN_STOP_OUTPUT;
		data->REG_FAN_STOP_TIME = NCT6775_REG_FAN_STOP_TIME;
		data->REG_FAN_MAX_OUTPUT = NCT6775_REG_FAN_MAX_OUTPUT;
		data->REG_FAN_STEP_OUTPUT = NCT6775_REG_FAN_STEP_OUTPUT;
	} else if (sio_data->kind == nct6776) {
		data->has_fan_div = false;
		data->fan_from_reg = fan_from_reg13;
		data->fan_from_reg_min = fan_from_reg13;
		data->REG_PWM = NCT6775_REG_PWM;
		data->REG_TARGET = NCT6775_REG_TARGET;
		data->REG_FAN = NCT6775_REG_FAN;
		data->REG_FAN_MIN = NCT6776_REG_FAN_MIN;
		data->REG_FAN_START_OUTPUT = NCT6775_REG_FAN_START_OUTPUT;
		data->REG_FAN_STOP_OUTPUT = NCT6775_REG_FAN_STOP_OUTPUT;
		data->REG_FAN_STOP_TIME = NCT6775_REG_FAN_STOP_TIME;
	} else if (sio_data->kind == w83667hg_b) {
		data->has_fan_div = true;
		data->fan_from_reg = fan_from_reg8;
		data->fan_from_reg_min = fan_from_reg8;
		data->REG_PWM = W83627EHF_REG_PWM;
		data->REG_TARGET = W83627EHF_REG_TARGET;
		data->REG_FAN = W83627EHF_REG_FAN;
		data->REG_FAN_MIN = W83627EHF_REG_FAN_MIN;
		data->REG_FAN_START_OUTPUT = W83627EHF_REG_FAN_START_OUTPUT;
		data->REG_FAN_STOP_OUTPUT = W83627EHF_REG_FAN_STOP_OUTPUT;
		data->REG_FAN_STOP_TIME = W83627EHF_REG_FAN_STOP_TIME;
		data->REG_FAN_MAX_OUTPUT =
		  W83627EHF_REG_FAN_MAX_OUTPUT_W83667_B;
		data->REG_FAN_STEP_OUTPUT =
		  W83627EHF_REG_FAN_STEP_OUTPUT_W83667_B;
	} else {
		data->has_fan_div = true;
		data->fan_from_reg = fan_from_reg8;
		data->fan_from_reg_min = fan_from_reg8;
		data->REG_PWM = W83627EHF_REG_PWM;
		data->REG_TARGET = W83627EHF_REG_TARGET;
		data->REG_FAN = W83627EHF_REG_FAN;
		data->REG_FAN_MIN = W83627EHF_REG_FAN_MIN;
		data->REG_FAN_START_OUTPUT = W83627EHF_REG_FAN_START_OUTPUT;
		data->REG_FAN_STOP_OUTPUT = W83627EHF_REG_FAN_STOP_OUTPUT;
		data->REG_FAN_STOP_TIME = W83627EHF_REG_FAN_STOP_TIME;
		data->REG_FAN_MAX_OUTPUT =
		  W83627EHF_REG_FAN_MAX_OUTPUT_COMMON;
		data->REG_FAN_STEP_OUTPUT =
		  W83627EHF_REG_FAN_STEP_OUTPUT_COMMON;
	}

	/* Setup input voltage scaling factors */
	if (sio_data->kind == w83627uhg)
		data->scale_in = scale_in_w83627uhg;
	else
		data->scale_in = scale_in_common;

	/* Initialize the chip */
	w83627ehf_init_device(data, sio_data->kind);

	data->vrm = vid_which_vrm();

	err = superio_enter(sio_data->sioreg);
	if (err)
		goto exit_release;

	/* Read VID value */
	if (sio_data->kind == w83667hg || sio_data->kind == w83667hg_b ||
	    sio_data->kind == nct6775 || sio_data->kind == nct6776) {
		/*
		 * W83667HG has different pins for VID input and output, so
		 * we can get the VID input values directly at logical device D
		 * 0xe3.
		 */
		superio_select(sio_data->sioreg, W83667HG_LD_VID);
		data->vid = superio_inb(sio_data->sioreg, 0xe3);
	} else if (sio_data->kind != w83627uhg) {
		superio_select(sio_data->sioreg, W83627EHF_LD_HWM);
		if (superio_inb(sio_data->sioreg, SIO_REG_VID_CTRL) & 0x80) {
			/*
			 * Set VID input sensibility if needed. In theory the
			 * BIOS should have set it, but in practice it's not
			 * always the case. We only do it for the W83627EHF/EHG
			 * because the W83627DHG is more complex in this
			 * respect.
			 */
			if (sio_data->kind == w83627ehf) {
				en_vrm10 = superio_inb(sio_data->sioreg,
						       SIO_REG_EN_VRM10);
				if ((en_vrm10 & 0x08) && data->vrm == 90) {
					dev_warn(dev,
						 "Setting VID input voltage to TTL\n");
					superio_outb(sio_data->sioreg,
						     SIO_REG_EN_VRM10,
						     en_vrm10 & ~0x08);
				} else if (!(en_vrm10 & 0x08)
					   && data->vrm == 100) {
					dev_warn(dev,
						 "Setting VID input voltage to VRM10\n");
					superio_outb(sio_data->sioreg,
						     SIO_REG_EN_VRM10,
						     en_vrm10 | 0x08);
				}
			}

			data->vid = superio_inb(sio_data->sioreg,
						SIO_REG_VID_DATA);
			if (sio_data->kind == w83627ehf) /* 6 VID pins only */
				data->vid &= 0x3f;

		} else {
			dev_info(dev,
				 "VID pins in output mode, CPU VID not available\n");
		}
	}

	if (fan_debounce &&
	    (sio_data->kind == nct6775 || sio_data->kind == nct6776)) {
		u8 tmp;

		superio_select(sio_data->sioreg, W83627EHF_LD_HWM);
		tmp = superio_inb(sio_data->sioreg, NCT6775_REG_FAN_DEBOUNCE);
		if (sio_data->kind == nct6776)
			superio_outb(sio_data->sioreg, NCT6775_REG_FAN_DEBOUNCE,
				     0x3e | tmp);
		else
			superio_outb(sio_data->sioreg, NCT6775_REG_FAN_DEBOUNCE,
				     0x1e | tmp);
		pr_info("Enabled fan debounce for chip %s\n", data->name);
	}

	w83627ehf_check_fan_inputs(sio_data, data);

	superio_exit(sio_data->sioreg);

	/* Read fan clock dividers immediately */
	w83627ehf_update_fan_div_common(dev, data);

	/* Read pwm data to save original values */
	w83627ehf_update_pwm_common(dev, data);
	for (i = 0; i < data->pwm_num; i++)
		data->pwm_enable_orig[i] = data->pwm_enable[i];

	data->hwmon_dev = hwmon_device_register_with_info(dev, data->name,
							  data,
							  &w83627ehf_chip_info,
							  sensor_groups);
	if (IS_ERR(data->hwmon_dev)) {
		err = PTR_ERR(data->hwmon_dev);
		goto exit_release;
	}

	return 0;

exit_release:
	release_region(res->start, IOREGION_LENGTH);
exit:
	return err;
}

static int w83627ehf_remove(struct platform_device *pdev)
{
	struct w83627ehf_data *data = platform_get_drvdata(pdev);

	hwmon_device_unregister(data->hwmon_dev);
	release_region(data->addr, IOREGION_LENGTH);

	return 0;
}

#ifdef CONFIG_PM
static int w83627ehf_suspend(struct device *dev)
{
	struct w83627ehf_data *data = w83627ehf_update_device(dev);
	struct w83627ehf_sio_data *sio_data = data->sio_data;

	mutex_lock(&data->update_lock);
	data->vbat = w83627ehf_read_value(data, W83627EHF_REG_VBAT);
	if (sio_data->kind == nct6775) {
		data->fandiv1 = w83627ehf_read_value(data, NCT6775_REG_FANDIV1);
		data->fandiv2 = w83627ehf_read_value(data, NCT6775_REG_FANDIV2);
	}
	mutex_unlock(&data->update_lock);

	return 0;
}

static int w83627ehf_resume(struct device *dev)
{
	struct w83627ehf_data *data = dev_get_drvdata(dev);
	struct w83627ehf_sio_data *sio_data = data->sio_data;
	int i;

	mutex_lock(&data->update_lock);
	data->bank = 0xff;		/* Force initial bank selection */

	/* Restore limits */
	for (i = 0; i < data->in_num; i++) {
		if ((i == 6) && data->in6_skip)
			continue;

		w83627ehf_write_value(data, W83627EHF_REG_IN_MIN(i),
				      data->in_min[i]);
		w83627ehf_write_value(data, W83627EHF_REG_IN_MAX(i),
				      data->in_max[i]);
	}

	for (i = 0; i < 5; i++) {
		if (!(data->has_fan_min & (1 << i)))
			continue;

		w83627ehf_write_value(data, data->REG_FAN_MIN[i],
				      data->fan_min[i]);
	}

	for (i = 0; i < NUM_REG_TEMP; i++) {
		if (!(data->have_temp & (1 << i)))
			continue;

		if (data->reg_temp_over[i])
			w83627ehf_write_temp(data, data->reg_temp_over[i],
					     data->temp_max[i]);
		if (data->reg_temp_hyst[i])
			w83627ehf_write_temp(data, data->reg_temp_hyst[i],
					     data->temp_max_hyst[i]);
		if (i > 2)
			continue;
		if (data->have_temp_offset & (1 << i))
			w83627ehf_write_value(data,
					      W83627EHF_REG_TEMP_OFFSET[i],
					      data->temp_offset[i]);
	}

	/* Restore other settings */
	w83627ehf_write_value(data, W83627EHF_REG_VBAT, data->vbat);
	if (sio_data->kind == nct6775) {
		w83627ehf_write_value(data, NCT6775_REG_FANDIV1, data->fandiv1);
		w83627ehf_write_value(data, NCT6775_REG_FANDIV2, data->fandiv2);
	}

	/* Force re-reading all values */
	data->valid = 0;
	mutex_unlock(&data->update_lock);

	return 0;
}

static const struct dev_pm_ops w83627ehf_dev_pm_ops = {
	.suspend = w83627ehf_suspend,
	.resume = w83627ehf_resume,
	.freeze = w83627ehf_suspend,
	.restore = w83627ehf_resume,
};

#define W83627EHF_DEV_PM_OPS	(&w83627ehf_dev_pm_ops)
#else
#define W83627EHF_DEV_PM_OPS	NULL
#endif /* CONFIG_PM */

static struct platform_driver w83627ehf_driver = {
	.driver = {
		.name	= DRVNAME,
		.pm	= W83627EHF_DEV_PM_OPS,
	},
	.probe		= w83627ehf_probe,
	.remove		= w83627ehf_remove,
};

/* w83627ehf_find() looks for a '627 in the Super-I/O config space */
static int __init w83627ehf_find(int sioaddr, unsigned short *addr,
				 struct w83627ehf_sio_data *sio_data)
{
	static const char sio_name_W83627EHF[] __initconst = "W83627EHF";
	static const char sio_name_W83627EHG[] __initconst = "W83627EHG";
	static const char sio_name_W83627DHG[] __initconst = "W83627DHG";
	static const char sio_name_W83627DHG_P[] __initconst = "W83627DHG-P";
	static const char sio_name_W83627UHG[] __initconst = "W83627UHG";
	static const char sio_name_W83667HG[] __initconst = "W83667HG";
	static const char sio_name_W83667HG_B[] __initconst = "W83667HG-B";
	static const char sio_name_NCT6775[] __initconst = "NCT6775F";
	static const char sio_name_NCT6776[] __initconst = "NCT6776F";

	u16 val;
	const char *sio_name;
	int err;

	err = superio_enter(sioaddr);
	if (err)
		return err;

	if (force_id)
		val = force_id;
	else
		val = (superio_inb(sioaddr, SIO_REG_DEVID) << 8)
		    | superio_inb(sioaddr, SIO_REG_DEVID + 1);
	switch (val & SIO_ID_MASK) {
	case SIO_W83627EHF_ID:
		sio_data->kind = w83627ehf;
		sio_name = sio_name_W83627EHF;
		break;
	case SIO_W83627EHG_ID:
		sio_data->kind = w83627ehf;
		sio_name = sio_name_W83627EHG;
		break;
	case SIO_W83627DHG_ID:
		sio_data->kind = w83627dhg;
		sio_name = sio_name_W83627DHG;
		break;
	case SIO_W83627DHG_P_ID:
		sio_data->kind = w83627dhg_p;
		sio_name = sio_name_W83627DHG_P;
		break;
	case SIO_W83627UHG_ID:
		sio_data->kind = w83627uhg;
		sio_name = sio_name_W83627UHG;
		break;
	case SIO_W83667HG_ID:
		sio_data->kind = w83667hg;
		sio_name = sio_name_W83667HG;
		break;
	case SIO_W83667HG_B_ID:
		sio_data->kind = w83667hg_b;
		sio_name = sio_name_W83667HG_B;
		break;
	case SIO_NCT6775_ID:
		sio_data->kind = nct6775;
		sio_name = sio_name_NCT6775;
		break;
	case SIO_NCT6776_ID:
		sio_data->kind = nct6776;
		sio_name = sio_name_NCT6776;
		break;
	default:
		if (val != 0xffff)
			pr_debug("unsupported chip ID: 0x%04x\n", val);
		superio_exit(sioaddr);
		return -ENODEV;
	}

	/* We have a known chip, find the HWM I/O address */
	superio_select(sioaddr, W83627EHF_LD_HWM);
	val = (superio_inb(sioaddr, SIO_REG_ADDR) << 8)
	    | superio_inb(sioaddr, SIO_REG_ADDR + 1);
	*addr = val & IOREGION_ALIGNMENT;
	if (*addr == 0) {
		pr_err("Refusing to enable a Super-I/O device with a base I/O port 0\n");
		superio_exit(sioaddr);
		return -ENODEV;
	}

	/* Activate logical device if needed */
	val = superio_inb(sioaddr, SIO_REG_ENABLE);
	if (!(val & 0x01)) {
		pr_warn("Forcibly enabling Super-I/O. Sensor is probably unusable.\n");
		superio_outb(sioaddr, SIO_REG_ENABLE, val | 0x01);
	}

	superio_exit(sioaddr);
	pr_info("Found %s chip at %#x\n", sio_name, *addr);
	sio_data->sioreg = sioaddr;

	return 0;
}

/*
 * when Super-I/O functions move to a separate file, the Super-I/O
 * bus will manage the lifetime of the device and this module will only keep
 * track of the w83627ehf driver. But since we platform_device_alloc(), we
 * must keep track of the device
 */
static struct platform_device *pdev;

static int __init sensors_w83627ehf_init(void)
{
	int err;
	unsigned short address;
	struct resource res;
	struct w83627ehf_sio_data sio_data;

	/*
	 * initialize sio_data->kind and sio_data->sioreg.
	 *
	 * when Super-I/O functions move to a separate file, the Super-I/O
	 * driver will probe 0x2e and 0x4e and auto-detect the presence of a
	 * w83627ehf hardware monitor, and call probe()
	 */
	if (w83627ehf_find(0x2e, &address, &sio_data) &&
	    w83627ehf_find(0x4e, &address, &sio_data))
		return -ENODEV;

	err = platform_driver_register(&w83627ehf_driver);
	if (err)
		goto exit;

	pdev = platform_device_alloc(DRVNAME, address);
	if (!pdev) {
		err = -ENOMEM;
		pr_err("Device allocation failed\n");
		goto exit_unregister;
	}

	err = platform_device_add_data(pdev, &sio_data,
				       sizeof(struct w83627ehf_sio_data));
	if (err) {
		pr_err("Platform data allocation failed\n");
		goto exit_device_put;
	}

	memset(&res, 0, sizeof(res));
	res.name = DRVNAME;
	res.start = address + IOREGION_OFFSET;
	res.end = address + IOREGION_OFFSET + IOREGION_LENGTH - 1;
	res.flags = IORESOURCE_IO;

	err = acpi_check_resource_conflict(&res);
	if (err)
		goto exit_device_put;

	err = platform_device_add_resources(pdev, &res, 1);
	if (err) {
		pr_err("Device resource addition failed (%d)\n", err);
		goto exit_device_put;
	}

	/* platform_device_add calls probe() */
	err = platform_device_add(pdev);
	if (err) {
		pr_err("Device addition failed (%d)\n", err);
		goto exit_device_put;
	}

	return 0;

exit_device_put:
	platform_device_put(pdev);
exit_unregister:
	platform_driver_unregister(&w83627ehf_driver);
exit:
	return err;
}

static void __exit sensors_w83627ehf_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&w83627ehf_driver);
}

MODULE_AUTHOR("Jean Delvare <jdelvare@suse.de>");
MODULE_DESCRIPTION("W83627EHF driver");
MODULE_LICENSE("GPL");

module_init(sensors_w83627ehf_init);
module_exit(sensors_w83627ehf_exit);
