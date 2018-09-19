// SPDX-License-Identifier: GPL-2.0+
/*
 * DA7280 Haptic device driver
 *
 * Copyright (c) 2018 Dialog Semiconductor.
 * Author: Roy Im <Roy.Im.Opensource@diasemi.com>
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include "da7280.h"

/* uV unit for voltage rate */
#define DA7280_VOLTAGE_RATE_MAX		6000000
#define DA7280_VOLTAGE_RATE_STEP	23400
#define DA7280_NOMMAX_DFT		0x6B
#define DA7280_ABSMAX_DFT		0x78

#define DA7280_IMPD_MAX			1500000000
#define DA7280_IMPD_DEFAULT		22000000

#define DA7280_IMAX_DEFAULT		0x0E
/* uA unit step and limit for IMAX*/
#define DA7280_IMAX_STEP		7200
#define DA7280_IMAX_LIMIT		252000

#define DA7280_RESONT_FREQH_DFT		0x39
#define DA7280_RESONT_FREQL_DFT		0x32
#define DA7280_MIN_RESONAT_FREQ_HZ	50
#define DA7280_MAX_RESONAT_FREQ_HZ	300
#define DA7280_MIN_PWM_FREQ_KHZ		10
#define DA7280_MAX_PWM_FREQ_KHZ		250

#define DA7280_SEQ_ID_MAX		15
#define DA7280_SEQ_LOOP_MAX		15
#define DA7280_GPI1_SEQ_ID_DEFT	0x0

#define DA7280_SNP_MEM_SIZE		100
#define DA7280_SNP_MEM_MAX		DA7280_SNP_MEM_99

#define IRQ_NUM				3

#define DA7280_SKIP_INIT		0x100

enum da7280_haptic_dev_t {
	DA7280_LRA	= 0,
	DA7280_ERM_BAR	= 1,
	DA7280_ERM_COIN	= 2,
	DA7280_DEV_MAX,
};

enum da7280_op_mode {
	DA7280_INACTIVE		= 0,
	DA7280_DRO_MODE		= 1,
	DA7280_PWM_MODE		= 2,
	DA7280_RTWM_MODE	= 3,
	DA7280_ETWM_MODE	= 4,
	DA7280_OPMODE_MAX,
};

struct da7280_gpi_ctl {
	u8 seq_id;
	u8 mode;
	u8 polarity;
};

struct da7280_haptic {
	struct regmap *regmap;
	struct input_dev *input_dev;
	struct device *dev;
	struct i2c_client *client;
	struct pwm_device *pwm_dev;
	bool	legacy;
	int pwm_id;
	struct work_struct work;

	bool suspend_state;
	unsigned int magnitude;

	u8 dev_type;
	u8 op_mode;
	u16 nommax;
	u16 absmax;
	u32 imax;
	u32 impd;
	u32 resonant_freq_h;
	u32 resonant_freq_l;
	u8 bemf_sense_en;
	u8 freq_track_en;
	u8 acc_en;
	u8 rapid_stop_en;
	u8 amp_pid_en;
	u8 ps_seq_id;
	u8 ps_seq_loop;
	struct da7280_gpi_ctl gpi_ctl[3];
	bool mem_update;
	u8 snp_mem[DA7280_SNP_MEM_SIZE];
	const struct attribute_group **attr_group;
};

static bool da7280_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case DA7280_IRQ_EVENT1:
	case DA7280_IRQ_EVENT_WARNING_DIAG:
	case DA7280_IRQ_EVENT_SEQ_DIAG:
	case DA7280_IRQ_STATUS1:
	case DA7280_TOP_CTL1:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config da7280_haptic_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = DA7280_SNP_MEM_MAX,
	.volatile_reg = da7280_volatile_register,
};

static int da7280_haptic_mem_update(struct da7280_haptic *haptics)
{
	int ret;
	unsigned int val;

	/* It is recommended to update the patterns
	 * during haptic is not working in order to avoid conflict
	 */
	ret = regmap_read(haptics->regmap, DA7280_IRQ_STATUS1, &val);
	if (ret)
		return ret;
	if (val & DA7280_STA_WARNING_MASK) {
		dev_warn(haptics->dev,
			 "Warning! Please check HAPTIC status.\n");
		return -EBUSY;
	}

	/* Patterns are not updated if the lock bit is enabled */
	val = 0;
	ret = regmap_read(haptics->regmap, DA7280_MEM_CTL2, &val);
	if (ret)
		return ret;
	if (~val & DA7280_WAV_MEM_LOCK_MASK) {
		dev_warn(haptics->dev,
			 "Please unlock the bit first\n");
		return -EACCES;
	}

	/* Set to Inactive mode to make sure safety */
	ret = regmap_update_bits(haptics->regmap,
				 DA7280_TOP_CTL1,
				 DA7280_OPERATION_MODE_MASK,
				 0);
	if (ret)
		return ret;

	ret = regmap_read(haptics->regmap, DA7280_MEM_CTL1, &val);
	if (ret)
		return ret;

	return regmap_bulk_write(haptics->regmap, val,
			haptics->snp_mem, DA7280_SNP_MEM_MAX - val + 1);
}

static int da7280_haptic_set_pwm(struct da7280_haptic *haptics)
{
	struct pwm_args pargs;
	u64 period_mag_multi;
	unsigned int pwm_duty;
	int ret;

	pwm_get_args(haptics->pwm_dev, &pargs);
	period_mag_multi =
		(u64)(pargs.period * haptics->magnitude);
	if (haptics->acc_en)
		pwm_duty =
			(unsigned int)(period_mag_multi >> 16);
	else
		pwm_duty =
			(unsigned int)((period_mag_multi >> 16)
				+ pargs.period) / 2;

	ret = pwm_config(haptics->pwm_dev,
			 pwm_duty, pargs.period);
	if (ret) {
		dev_err(haptics->dev,
			"failed to configure pwm : %d\n", ret);
		return ret;
	}

	ret = pwm_enable(haptics->pwm_dev);
	if (ret) {
		pwm_disable(haptics->pwm_dev);
		dev_err(haptics->dev,
			"failed to enable haptics pwm device : %d\n", ret);
	}

	return ret;
}

static void da7280_haptic_enable(struct da7280_haptic *haptics)
{
	int ret = 0;

	switch (haptics->op_mode) {
	case DA7280_DRO_MODE:
		/* the valid range check when acc_en is enabled */
		if (haptics->acc_en && haptics->magnitude > 0x7F)
			haptics->magnitude = 0x7F;
		else if (haptics->magnitude > 0xFF)
			haptics->magnitude = 0xFF;

		/* Set driver level
		 * as a % of ACTUATOR_NOMMAX(nommax)
		 */
		ret = regmap_write(haptics->regmap,
				   DA7280_TOP_CTL2,
				   haptics->magnitude);
		if (ret) {
			dev_err(haptics->dev,
				"i2c err for driving level set : %d\n",
				ret);
			return;
		}
		break;
	case DA7280_PWM_MODE:
		if (da7280_haptic_set_pwm(haptics))
			return;
		break;
	case DA7280_RTWM_MODE:
		/* PS_SEQ_ID will be played
		 * as many times as the PS_SEQ_LOOP
		 */
	case DA7280_ETWM_MODE:
		/* Now users are able to control the GPI(N)
		 * assigned to GPI_0, GPI1 and GPI2 accordingly
		 * please see the datasheet for details.
		 * GPI(N)_SEQUENCE_ID will be played
		 * as many times as the PS_SEQ_LOOP
		 */
		break;
	default:
		dev_err(haptics->dev,
			"Invalid Mode(%d)\n", haptics->op_mode);
		return;
	}

	ret = regmap_update_bits(haptics->regmap,
				 DA7280_TOP_CTL1,
				 DA7280_OPERATION_MODE_MASK,
				 haptics->op_mode);
	if (ret) {
		dev_err(haptics->dev,
			"i2c err for op_mode setting : %d\n", ret);
		return;
	}

	if (haptics->op_mode == DA7280_PWM_MODE ||
	    haptics->op_mode == DA7280_RTWM_MODE) {
		ret = regmap_update_bits(haptics->regmap,
					 DA7280_TOP_CTL1,
					 DA7280_SEQ_START_MASK,
					 DA7280_SEQ_START_MASK);
		if (ret)
			dev_err(haptics->dev,
				"i2c err for sequence triggering : %d\n", ret);
	}
}

static void da7280_haptic_disable(struct da7280_haptic *haptics)
{
	int ret;

	/* Set to Inactive mode */
	ret = regmap_update_bits(haptics->regmap,
				 DA7280_TOP_CTL1,
				 DA7280_OPERATION_MODE_MASK, 0);
	if (ret) {
		dev_err(haptics->dev,
			"i2c err for op_mode off : %d\n", ret);
		return;
	}

	switch (haptics->op_mode) {
	case DA7280_RTWM_MODE:
	case DA7280_ETWM_MODE:
		ret = regmap_update_bits(haptics->regmap,
					 DA7280_TOP_CTL1,
					 DA7280_SEQ_START_MASK, 0);
		if (ret) {
			dev_err(haptics->dev,
				"i2c err for RTWM or ETWM mode off : %d\n",
				ret);
			return;
		}
		break;
	case DA7280_DRO_MODE:
		ret = regmap_write(haptics->regmap,
				   DA7280_TOP_CTL2, 0);
		if (ret) {
			dev_err(haptics->dev,
				"i2c err for DRO mode off : %d\n",
				ret);
			return;
		}
		break;
	case DA7280_PWM_MODE:
		pwm_disable(haptics->pwm_dev);
		break;
	default:
		dev_err(haptics->dev,
			"Invalid Mode(%d)\n", haptics->op_mode);
		break;
	}
}

static void da7280_haptic_work(struct work_struct *work)
{
	struct da7280_haptic *haptics =
			container_of(work, struct da7280_haptic, work);

	if (haptics->magnitude)
		da7280_haptic_enable(haptics);
	else
		da7280_haptic_disable(haptics);
}

static int da7280_haptic_play(struct input_dev *dev, void *data,
			      struct ff_effect *effect)
{
	struct da7280_haptic *haptics = input_get_drvdata(dev);

	if (effect->u.rumble.strong_magnitude > 0)
		haptics->magnitude = effect->u.rumble.strong_magnitude;
	else if (effect->u.rumble.weak_magnitude > 0)
		haptics->magnitude = effect->u.rumble.weak_magnitude;
	else
		haptics->magnitude = 0;

	schedule_work(&haptics->work);

	return 0;
}

static int da7280_haptic_open(struct input_dev *dev)
{
	struct da7280_haptic *haptics = input_get_drvdata(dev);
	int ret;

	ret = regmap_update_bits(haptics->regmap,
				 DA7280_TOP_CTL1,
				 DA7280_STANDBY_EN_MASK,
				 DA7280_STANDBY_EN_MASK);
	if (ret)
		dev_err(haptics->dev,
			"Failed to open haptic, i2c error : %d\n", ret);

	return ret;
}

static void da7280_haptic_close(struct input_dev *dev)
{
	struct da7280_haptic *haptics = input_get_drvdata(dev);
	int ret;

	cancel_work_sync(&haptics->work);

	ret = regmap_update_bits(haptics->regmap,
				 DA7280_TOP_CTL1,
				 DA7280_OPERATION_MODE_MASK, 0);
	if (ret)
		goto error_i2c;

	if (haptics->op_mode == DA7280_DRO_MODE) {
		ret = regmap_write(haptics->regmap,
				   DA7280_TOP_CTL2, 0);

		if (ret)
			goto error_i2c;
	}

	ret = regmap_update_bits(haptics->regmap,
				 DA7280_TOP_CTL1,
				 DA7280_STANDBY_EN_MASK, 0);
	if (ret)
		goto error_i2c;

	return;

error_i2c:
	dev_err(haptics->dev, "DA7280-haptic i2c error : %d\n", ret);
}

static u8 da7280_haptic_of_mode_str(struct device *dev,
				    const char *str)
{
	if (!strcmp(str, "LRA"))
		return DA7280_LRA;
	else if (!strcmp(str, "ERM-bar"))
		return DA7280_ERM_BAR;
	else if (!strcmp(str, "ERM-coin"))
		return DA7280_ERM_COIN;

	dev_warn(dev, "Invalid string - set to default\n");
	return DA7280_LRA;
}

static u8 da7280_haptic_of_gpi_mode_str(struct device *dev,
					const char *str)
{
	if (!strcmp(str, "Single-pattern"))
		return 0;
	else if (!strcmp(str, "Multi-pattern"))
		return 1;

	dev_warn(dev, "Invalid string - set to default\n");
	return 0;
}

static u8 da7280_haptic_of_gpi_pol_str(struct device *dev,
				       const char *str)
{
	if (!strcmp(str, "Rising-edge"))
		return 0;
	else if (!strcmp(str, "Falling-edge"))
		return 1;
	else if (!strcmp(str, "Both-edge"))
		return 2;

	dev_warn(dev, "Invalid string - set to default\n");
	return 0;
}

static u8 da7280_haptic_of_volt_rating_set(u32 val)
{
	u32 voltage;

	voltage = val / DA7280_VOLTAGE_RATE_STEP + 1;

	if (voltage > 0xFF)
		return 0xFF;
	return (u8)voltage;
}

static void da7280_of_to_pdata(struct device *dev,
			       struct da7280_haptic *haptics)
{
	struct device_node *np = dev->of_node;
	char dt_gpi_str1[] = "dlg,gpi0-seq-id";
	char dt_gpi_str2[] = "dlg,gpi0-mode";
	char dt_gpi_str3[] = "dlg,gpi0-polarity";
	unsigned int mem[DA7280_SNP_MEM_SIZE];
	const char *of_str;
	u32 of_val32;
	int i;

	if (!of_property_read_string(np, "dlg,actuator-type", &of_str))
		haptics->dev_type =
			da7280_haptic_of_mode_str(dev, of_str);
	else /* if no property, then use the mode inside chip */
		haptics->dev_type = DA7280_DEV_MAX;

	if (of_property_read_u32(np, "dlg,op-mode", &of_val32) >= 0)
		if (of_val32 && of_val32 < DA7280_OPMODE_MAX)
			haptics->op_mode = of_val32;
		else
			haptics->op_mode = DA7280_DRO_MODE;
	else
		haptics->op_mode = DA7280_DRO_MODE;

	if (of_property_read_u32(np, "dlg,nom-microvolt", &of_val32) >= 0)
		if (of_val32 < DA7280_VOLTAGE_RATE_MAX)
			haptics->nommax =
				da7280_haptic_of_volt_rating_set(of_val32);
		else
			haptics->nommax = DA7280_SKIP_INIT;
	else /* if no property, then use the value inside chip */
		haptics->nommax = DA7280_SKIP_INIT;

	if (of_property_read_u32(np, "dlg,abs-max-microvolt", &of_val32) >= 0)
		if (of_val32 < DA7280_VOLTAGE_RATE_MAX)
			haptics->absmax =
				da7280_haptic_of_volt_rating_set(of_val32);
		else
			haptics->absmax = DA7280_SKIP_INIT;
	else
		haptics->absmax = DA7280_SKIP_INIT;

	if (of_property_read_u32(np, "dlg,imax-microamp", &of_val32) >= 0)
		if (of_val32 < DA7280_IMAX_LIMIT)
			haptics->imax = (of_val32 - 28600)
					/ DA7280_IMAX_STEP + 1;
		else
			haptics->imax = DA7280_IMAX_DEFAULT;
	else
		haptics->imax = DA7280_IMAX_DEFAULT;

	if (of_property_read_u32(np, "dlg,impd-micro-ohms", &of_val32) >= 0)
		if (of_val32 <= DA7280_IMPD_MAX)
			haptics->impd = of_val32;
		else
			haptics->impd = DA7280_IMPD_DEFAULT;
	else
		haptics->impd = DA7280_IMPD_DEFAULT;

	if (of_property_read_u32(np, "dlg,resonant-freq-hz", &of_val32) >= 0) {
		if (of_val32 < DA7280_MAX_RESONAT_FREQ_HZ &&
		    of_val32 > DA7280_MIN_RESONAT_FREQ_HZ) {
			haptics->resonant_freq_h =
				((1000000000 / (of_val32 * 1333)) >> 7) & 0xFF;
			haptics->resonant_freq_l =
				(1000000000 / (of_val32 * 1333)) & 0x7F;
		} else {
			haptics->resonant_freq_h =
				DA7280_RESONT_FREQH_DFT;
			haptics->resonant_freq_l =
				DA7280_RESONT_FREQL_DFT;
		}
	} else {
		haptics->resonant_freq_h = DA7280_SKIP_INIT;
		haptics->resonant_freq_l = DA7280_SKIP_INIT;
	}

	if (of_property_read_u32(np, "dlg,ps-seq-id", &of_val32) >= 0)
		if (of_val32 <= DA7280_SEQ_ID_MAX)
			haptics->ps_seq_id = of_val32;
		else
			haptics->ps_seq_id = 0;
	else /* if no property, set to zero as a default do nothing */
		haptics->ps_seq_id = 0;

	if (of_property_read_u32(np, "dlg,ps-seq-loop", &of_val32) >= 0)
		if (of_val32 <= DA7280_SEQ_LOOP_MAX)
			haptics->ps_seq_loop = of_val32;
		else
			haptics->ps_seq_loop = 0;
	else /* if no property, then do nothing */
		haptics->ps_seq_loop = 0;

	/* GPI0~2 Control */
	for (i = 0; i < 3; i++) {
		dt_gpi_str1[7] = '0' + i;
		if (of_property_read_u32(np, dt_gpi_str1, &of_val32) >= 0)
			if (of_val32 <= DA7280_SEQ_ID_MAX)
				haptics->gpi_ctl[i].seq_id = of_val32;
			else
				haptics->gpi_ctl[i].seq_id =
					DA7280_GPI1_SEQ_ID_DEFT + i;
		else /* if no property, then do nothing */
			haptics->gpi_ctl[i].seq_id =
				DA7280_GPI1_SEQ_ID_DEFT + i;

		dt_gpi_str2[7] = '0' + i;
		if (!of_property_read_string(np, dt_gpi_str2, &of_str))
			haptics->gpi_ctl[i].mode =
				da7280_haptic_of_gpi_mode_str(dev, of_str);
		else
			haptics->gpi_ctl[i].mode = 0;

		dt_gpi_str3[7] = '0' + i;
		if (!of_property_read_string(np, dt_gpi_str3, &of_str))
			haptics->gpi_ctl[i].polarity =
				da7280_haptic_of_gpi_pol_str(dev, of_str);
		else
			haptics->gpi_ctl[i].polarity = 0;
	}

	haptics->bemf_sense_en =
		of_property_read_bool(np, "dlg,bemf-sens-enable");
	haptics->freq_track_en =
		of_property_read_bool(np, "dlg,freq-track-enable");
	haptics->acc_en =
		of_property_read_bool(np, "dlg,acc-enable");
	haptics->rapid_stop_en =
		of_property_read_bool(np, "dlg,rapid-stop-enable");
	haptics->amp_pid_en =
		of_property_read_bool(np, "dlg,amp-pid-enable");

	if (of_property_read_u32_array(np, "dlg,mem-array",
				       &mem[0], DA7280_SNP_MEM_SIZE) >= 0) {
		haptics->mem_update = 1;
		for (i = 0; i < DA7280_SNP_MEM_SIZE; i++) {
			if (mem[i] > 0xff)
				haptics->snp_mem[i] = 0x0;
			else
				haptics->snp_mem[i] = (u8)mem[i];
		}
	} else {
		haptics->mem_update = 0;
	}
}

static irqreturn_t da7280_irq_handler(int irq, void *data)
{
	struct da7280_haptic *haptics = data;
	u8 events[IRQ_NUM];
	int ret;

	/* Check what events have happened */
	ret = regmap_bulk_read(haptics->regmap,
			       DA7280_IRQ_EVENT1,
			       events, IRQ_NUM);
	if (ret)
		goto error_i2c;

	/* Empty check due to shared interrupt */
	if ((events[0] | events[1] | events[2]) == 0x00)
		return IRQ_HANDLED;

	if (events[0] & DA7280_E_SEQ_FAULT_MASK) {
		/* Stop first if Haptic is working
		 * Otherwise, the fault may happen continually
		 * even though the bit is cleared.
		 */
		ret = regmap_update_bits(haptics->regmap,
					 DA7280_TOP_CTL1,
					 DA7280_OPERATION_MODE_MASK, 0);
		if (ret)
			goto error_i2c;
	}

	/* Clear events */
	ret = regmap_write(haptics->regmap,
			   DA7280_IRQ_EVENT1, events[0]);
	if (ret)
		goto error_i2c;

	return IRQ_HANDLED;

error_i2c:
	dev_err(haptics->dev, "da7280 i2c error : %d\n", ret);
	return IRQ_NONE;
}

static int da7280_init(struct da7280_haptic *haptics)
{
	int ret, i;
	unsigned int val = 0;
	u32 v2i_factor;
	u8 mask = 0;

	/* If device type is DA7280_DEV_MAX,
	 * then just use default value inside chip.
	 */
	if (haptics->dev_type == DA7280_DEV_MAX) {
		ret = regmap_read(haptics->regmap, DA7280_TOP_CFG1, &val);
		if (ret)
			goto error_i2c;
		if (val & DA7280_ACTUATOR_TYPE_MASK)
			haptics->dev_type = DA7280_ERM_COIN;
		else
			haptics->dev_type = DA7280_LRA;
	}

	/* Apply user settings */
	if (haptics->dev_type == DA7280_LRA) {
		if (haptics->resonant_freq_l != DA7280_SKIP_INIT) {
			ret = regmap_write(haptics->regmap,
					   DA7280_FRQ_LRA_PER_H,
					   haptics->resonant_freq_h);
			if (ret)
				goto error_i2c;
			ret = regmap_write(haptics->regmap,
					   DA7280_FRQ_LRA_PER_L,
					   haptics->resonant_freq_l);
			if (ret)
				goto error_i2c;
		}
	} else if (haptics->dev_type == DA7280_ERM_COIN) {
		ret = regmap_update_bits(haptics->regmap,
					 DA7280_TOP_INT_CFG1,
					 DA7280_BEMF_FAULT_LIM_MASK, 0);
		if (ret)
			goto error_i2c;

		ret = regmap_update_bits(haptics->regmap,
					 DA7280_TOP_CFG4,
					 DA7280_TST_CALIB_IMPEDANCE_DIS_MASK |
					 DA7280_V2I_FACTOR_FREEZE_MASK,
					 DA7280_TST_CALIB_IMPEDANCE_DIS_MASK |
					 DA7280_V2I_FACTOR_FREEZE_MASK);
		if (ret)
			goto error_i2c;

		haptics->acc_en = 0;
		haptics->rapid_stop_en = 0;
		haptics->amp_pid_en = 0;
	}

	/* Should be set to 0 only
	 * in custom waveform and wideband operation
	 */
	if (haptics->op_mode >= DA7280_RTWM_MODE)
		haptics->bemf_sense_en = 0;

	mask = DA7280_ACTUATOR_TYPE_MASK |
			DA7280_BEMF_SENSE_EN_MASK |
			DA7280_FREQ_TRACK_EN_MASK |
			DA7280_ACCELERATION_EN_MASK |
			DA7280_RAPID_STOP_EN_MASK |
			DA7280_AMP_PID_EN_MASK;

	val = (haptics->dev_type ? 1 : 0)
			<< DA7280_ACTUATOR_TYPE_SHIFT |
		(haptics->bemf_sense_en ? 1 : 0)
			<< DA7280_BEMF_SENSE_EN_SHIFT |
		(haptics->freq_track_en ? 1 : 0)
			<< DA7280_FREQ_TRACK_EN_SHIFT |
		(haptics->acc_en ? 1 : 0)
			<< DA7280_ACCELERATION_EN_SHIFT |
		(haptics->rapid_stop_en ? 1 : 0)
			<< DA7280_RAPID_STOP_EN_SHIFT |
		(haptics->amp_pid_en ? 1 : 0)
			<< DA7280_AMP_PID_EN_SHIFT;

	ret = regmap_update_bits(haptics->regmap,
				 DA7280_TOP_CFG1, mask, val);
	if (ret)
		goto error_i2c;

	if (haptics->nommax != DA7280_SKIP_INIT) {
		ret = regmap_write(haptics->regmap,
				   DA7280_ACTUATOR1,
				   haptics->nommax);
		if (ret)
			goto error_i2c;
	}

	if (haptics->absmax != DA7280_SKIP_INIT) {
		ret = regmap_write(haptics->regmap, DA7280_ACTUATOR2,
				   haptics->absmax);
		if (ret)
			goto error_i2c;
	}

	ret = regmap_update_bits(haptics->regmap,
				 DA7280_ACTUATOR3,
				 DA7280_IMAX_MASK,
				 haptics->imax);
	if (ret)
		goto error_i2c;

	v2i_factor =
		haptics->impd * (haptics->imax + 4) / 1610400;
	ret = regmap_write(haptics->regmap,
			   DA7280_CALIB_V2I_L,
			   v2i_factor & 0xff);
	if (ret)
		goto error_i2c;
	ret = regmap_write(haptics->regmap,
			   DA7280_CALIB_V2I_H,
			   v2i_factor >> 8);
	if (ret)
		goto error_i2c;

	ret = regmap_update_bits(haptics->regmap,
				 DA7280_TOP_CTL1,
				 DA7280_STANDBY_EN_MASK,
				 DA7280_STANDBY_EN_MASK);
	if (ret)
		goto error_i2c;

	if (haptics->mem_update) {
		ret = da7280_haptic_mem_update(haptics);
		if (ret)
			goto error_i2c;
	}

	/* Set  PS_SEQ_ID and PS_SEQ_LOOP */
	val = haptics->ps_seq_id << DA7280_PS_SEQ_ID_SHIFT |
		haptics->ps_seq_loop << DA7280_PS_SEQ_LOOP_SHIFT;
	ret = regmap_write(haptics->regmap,
			   DA7280_SEQ_CTL2, val);
	if (ret)
		goto error_i2c;

	/* GPI(N) CTL */
	for (i = 0; i < 3; i++) {
		val = haptics->gpi_ctl[i].seq_id
				<< DA7280_GPI0_SEQUENCE_ID_SHIFT |
			haptics->gpi_ctl[i].mode
				<< DA7280_GPI0_MODE_SHIFT |
			haptics->gpi_ctl[i].polarity
				<< DA7280_GPI0_POLARITY_SHIFT;
		ret = regmap_write(haptics->regmap,
				   DA7280_GPI_0_CTL + i, val);
		if (ret)
			goto error_i2c;
	}

	/* Clear Interrupts */
	ret = regmap_write(haptics->regmap, DA7280_IRQ_EVENT1, 0xff);
	if (ret)
		goto error_i2c;

	ret = regmap_update_bits(haptics->regmap,
				 DA7280_IRQ_MASK1,
				 DA7280_SEQ_FAULT_M_MASK
				 | DA7280_SEQ_DONE_M_MASK, 0);
	if (ret)
		goto error_i2c;

	haptics->suspend_state = 0;
	return 0;

error_i2c:
	dev_err(haptics->dev, "haptic init - I2C error : %d\n", ret);
	return ret;
}

/* Valid format for ps_seq_id
 * echo X > ps_seq_id
 * ex) echo 2 > /sys/class/..../ps_seq_id
 * 0 <= X <= 15.
 */
static ssize_t ps_seq_id_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf,
			       size_t count)
{
	struct da7280_haptic *haptics = dev_get_drvdata(dev);
	long val = 0xff;
	int ret;

	if (kstrtol(&buf[0], 0, &val) < 0)
		goto err;

	ret = regmap_update_bits(haptics->regmap,
				 DA7280_SEQ_CTL2,
				 DA7280_PS_SEQ_ID_MASK,
				 (val & 0xf) >> DA7280_PS_SEQ_ID_SHIFT);
	if (ret) {
		dev_err(haptics->dev,
			"failed to update register : %d\n", ret);
		return ret;
	}

	haptics->ps_seq_id = val & 0xf;

	return count;

err:
	dev_err(dev, "Invalid input\n");
	return count;
}

static ssize_t ps_seq_id_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct da7280_haptic *haptics = dev_get_drvdata(dev);
	int ret;
	unsigned int val;

	ret = regmap_read(haptics->regmap, DA7280_SEQ_CTL2, &val);
	if (ret) {
		dev_err(haptics->dev,
			"failed to read register : %d\n", ret);
		return ret;
	}
	val = (val & DA7280_PS_SEQ_ID_MASK)
		>> DA7280_PS_SEQ_ID_SHIFT;

	return sprintf(buf, "ps_seq_id is %d\n", val);
}

/* Valid format for ps_seq_loop
 * echo X > ps_seq_loop
 * ex) echo 2 > /sys/class/..../ps_seq_loop
 * 0 <= X <= 15.
 */
static ssize_t ps_seq_loop_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf,
				 size_t count)
{
	struct da7280_haptic *haptics = dev_get_drvdata(dev);
	long val = 0xff;
	int ret;

	if (kstrtol(&buf[0], 0, &val) < 0)
		goto err;

	ret = regmap_update_bits(haptics->regmap,
				 DA7280_SEQ_CTL2,
				 DA7280_PS_SEQ_LOOP_MASK,
				 (val & 0xF) << DA7280_PS_SEQ_LOOP_SHIFT);
	if (ret) {
		dev_err(haptics->dev,
			"failed to update register : %d\n", ret);
		return ret;
	}

	haptics->ps_seq_loop = (val & 0xF);

	return count;
err:
	dev_err(dev, "Invalid input value!\n");
	return count;
}

static ssize_t ps_seq_loop_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct da7280_haptic *haptics = dev_get_drvdata(dev);
	int ret;
	unsigned int val;

	ret = regmap_read(haptics->regmap, DA7280_SEQ_CTL2, &val);
	if (ret) {
		dev_err(haptics->dev,
			"failed to read register : %d\n", ret);
		return ret;
	}
	val = (val & DA7280_PS_SEQ_LOOP_MASK)
				>> DA7280_PS_SEQ_LOOP_SHIFT;

	return sprintf(buf, "ps_seq_loop is %d\n", val);
}

/* Valid format for GPIx_SEQUENCE_ID
 * echo X Y > gpi_seq_id
 * ex) echo 2 15 > /sys/class/..../gpi_seq_id
 * 0 <= X < 3, 0<= Y <= 15.
 */
static ssize_t gpi_seq_id_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf,
				size_t count)
{
	struct da7280_haptic *haptics = dev_get_drvdata(dev);
	u8 gpi_num = 0xff;
	long seq_id = 0xff;
	int ret;

	if (count < 4)
		goto err;

	if (buf[0] >= '0')
		gpi_num = buf[0] - '0';
	else
		goto err;

	if (buf[1] != ' ')
		goto err;

	if (kstrtol(&buf[2], 0, &seq_id) < 0)
		goto err;

	if (gpi_num > 2 || seq_id > 0xf)
		goto err;

	ret = regmap_update_bits(haptics->regmap,
				 DA7280_GPI_0_CTL + gpi_num,
				 DA7280_GPI0_SEQUENCE_ID_MASK,
				 seq_id << DA7280_GPI0_SEQUENCE_ID_SHIFT);
	if (ret) {
		dev_err(haptics->dev,
			"failed to update register : %d\n", ret);
		return ret;
	}

	haptics->gpi_ctl[gpi_num].seq_id = seq_id;

	return count;

err:
	dev_err(dev, "Invalid format or values!\n");
	return count;
}

static ssize_t gpi_seq_id_show(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	struct da7280_haptic *haptics = dev_get_drvdata(dev);
	int ret;
	unsigned int val, i;

	for (i = 0; i < 3; i++) {
		ret = regmap_read(haptics->regmap,
				  DA7280_GPI_0_CTL + i, &val);
		if (ret) {
			dev_err(haptics->dev,
				"failed to read register : %d\n", ret);
			return ret;
		}
		haptics->gpi_ctl[i].seq_id =
			(val & DA7280_GPI0_SEQUENCE_ID_MASK)
				>> DA7280_GPI0_SEQUENCE_ID_SHIFT;
		val = 0;
	}

	return sprintf(buf,
		"Seq ID\nGPI0 : %d\nGPI1 : %d\nGPI2 : %d\n",
		haptics->gpi_ctl[0].seq_id,
		haptics->gpi_ctl[1].seq_id,
		haptics->gpi_ctl[2].seq_id);
}

/* Valid format for GPIx_MODE
 * echo X Y > gpi_mode
 * ex) echo 2 1 > /sys/class/..../gpi_mode
 * 0 <= X < 3, 0<= Y <= 1.
 */
static ssize_t gpi_mode_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct da7280_haptic *haptics = dev_get_drvdata(dev);
	u8 gpi_num = 0xff, gpi_mode = 0xff;
	int ret;

	if (count < 3)
		goto err;

	if (buf[0] >= '0')
		gpi_num = buf[0] - '0';
	if (buf[2] >= '0')
		gpi_mode = buf[2] - '0';

	if (gpi_num > 2 || gpi_mode > 1)
		goto err;

	ret = regmap_update_bits(haptics->regmap,
				 DA7280_GPI_0_CTL + gpi_num,
				 DA7280_GPI0_MODE_MASK,
				 gpi_mode << DA7280_GPI0_MODE_SHIFT);
	if (ret) {
		dev_err(haptics->dev,
			"failed to update register : %d\n", ret);
		return ret;
	}

	haptics->gpi_ctl[gpi_num].mode = gpi_mode;

	return count;

err:
	dev_err(dev, "Invalid format!\n");
	return count;
}

static ssize_t gpi_mode_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	struct da7280_haptic *haptics = dev_get_drvdata(dev);
	int ret;
	unsigned int val, i;

	for (i = 0; i < 3; i++) {
		ret = regmap_read(haptics->regmap,
				  DA7280_GPI_0_CTL + i, &val);
		if (ret) {
			dev_err(haptics->dev,
				"failed to read register : %d\n", ret);
			return ret;
		}
		haptics->gpi_ctl[i].mode =
			(val & DA7280_GPI0_MODE_MASK)
				>> DA7280_GPI0_MODE_SHIFT;
		val = 0;
	}

	return sprintf(buf, "Mode\nGPI0 : %d\nGPI1 : %d\nGPI2 : %d\n",
		haptics->gpi_ctl[0].mode,
		haptics->gpi_ctl[1].mode,
		haptics->gpi_ctl[2].mode);
}

/* Valid format for GPIx_MODE
 *  echo X Y > gpi_pol
 *  ex) echo 2 1 > /sys/class/..../gpi_pol
 *  0 <= X < 3, 0<= Y <= 2.
 */
static ssize_t gpi_pol_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf,
			     size_t count)
{
	struct da7280_haptic *haptics = dev_get_drvdata(dev);
	u8 gpi_pol = 0xff, gpi_num = 0xff;
	int ret;

	if (count < 3)
		goto err;

	if (buf[0] >= '0')
		gpi_num = buf[0] - '0';
	if (buf[2] >= '0')
		gpi_pol = buf[2] - '0';

	if (gpi_num > 2 || gpi_pol > 2)
		goto err;

	ret = regmap_update_bits(haptics->regmap,
				 DA7280_GPI_0_CTL + gpi_num,
				 DA7280_GPI0_POLARITY_MASK,
				 gpi_pol << DA7280_GPI0_POLARITY_SHIFT);
	if (ret) {
		dev_err(haptics->dev,
			"failed to update register : %d\n", ret);
		return ret;
	}

	haptics->gpi_ctl[gpi_num].polarity = gpi_pol;

	return count;

err:
	dev_err(dev, "Invalid format or input values!\n");
	return count;
}

static ssize_t gpi_pol_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	struct da7280_haptic *haptics = dev_get_drvdata(dev);
	int ret = 0;
	unsigned int val, i;

	for (i = 0; i < 3; i++) {
		ret = regmap_read(haptics->regmap,
				  DA7280_GPI_0_CTL + i, &val);
		if (ret)
			return ret;
		haptics->gpi_ctl[i].polarity =
			(val & DA7280_GPI0_POLARITY_MASK)
			>> DA7280_GPI0_POLARITY_SHIFT;
		val = 0;
	}

	return sprintf(buf, "Polarity\nGPI0 : %d\nGPI1 : %d\nGPI2 : %d\n",
		haptics->gpi_ctl[0].polarity,
		haptics->gpi_ctl[1].polarity,
		haptics->gpi_ctl[2].polarity);
}

#define MAX_PTN_REGS DA7280_SNP_MEM_SIZE
#define MAX_USER_INPUT_LEN (5 * DA7280_SNP_MEM_SIZE)
struct parse_data_t {
	int len;
	u8 val[MAX_PTN_REGS];
};

static int da7280_parse_args(struct device *dev,
			     char *cmd, struct parse_data_t *ptn)
{
	struct da7280_haptic *haptics = dev_get_drvdata(dev);
	char *tok;		/* used to separate tokens */
	const char ct[] = " \t";	/* space or tab delimits the tokens */
	int tok_count = 0;	/* total number of tokens parsed */
	int i = 0, val;

	ptn->len = 0;

	/* parse the input string */
	while ((tok = strsep(&cmd, ct)) != NULL) {
		/* this is a value to be written to the register */
		if (kstrtouint(tok, 0, &val) < 0) {
			dev_err(haptics->dev,
				"failed to read from %s\n", tok);
			break;
		}

		if (i < MAX_PTN_REGS) {
			ptn->val[i] = val;
			i++;
		}
		tok_count++;
	}

	/* decide whether it is a read or write operation based on the
	 * value of tok_count and count_flag.
	 * tok_count = 0: no inputs, invalid case.
	 * tok_count = 1: write one value.
	 * tok_count > 1: write multiple values/patterns.
	 */
	switch (tok_count) {
	case 0:
		return -EINVAL;
	case 1:
		ptn->len = 1;
		break;
	default:
		ptn->len = i;
	}
	return 0;
}

static ssize_t
patterns_store(struct device *dev,
	       struct device_attribute *attr,
	       const char *buf,
	       size_t count)
{
	struct da7280_haptic *haptics = dev_get_drvdata(dev);
	struct parse_data_t mem;
	char cmd[MAX_USER_INPUT_LEN];
	unsigned int val;
	int ret;

	ret = regmap_read(haptics->regmap, DA7280_MEM_CTL1, &val);
	if (ret)
		return ret;

	if (count > MAX_USER_INPUT_LEN)
		memcpy(cmd, buf, MAX_USER_INPUT_LEN);
	else
		memcpy(cmd, buf, count);
	/* chop of '\n' introduced by echo at the end of the input */
	if (cmd[count - 1] == '\n')
		cmd[count - 1] = '\0';

	if (da7280_parse_args(dev, cmd, &mem) < 0)
		return -EINVAL;

	memcpy(haptics->snp_mem, mem.val, mem.len);

	ret = da7280_haptic_mem_update(haptics);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(ps_seq_id);
static DEVICE_ATTR_RW(ps_seq_loop);
static DEVICE_ATTR_RW(gpi_seq_id);
static DEVICE_ATTR_RW(gpi_mode);
static DEVICE_ATTR_RW(gpi_pol);
static DEVICE_ATTR_WO(patterns);
static struct attribute *da7280_sysfs_attr[] = {
	&dev_attr_ps_seq_id.attr,
	&dev_attr_ps_seq_loop.attr,
	&dev_attr_gpi_seq_id.attr,
	&dev_attr_gpi_mode.attr,
	&dev_attr_gpi_pol.attr,
	&dev_attr_patterns.attr,
	NULL,
};

static const struct attribute_group da7280_attr_group = {
	.attrs = da7280_sysfs_attr,
};

static const struct attribute_group *da7280_attr_groups[] = {
	&da7280_attr_group,
	NULL,
};

static int da7280_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct da7280_haptic *haptics;
	unsigned int period2freq;
	int ret;

	haptics = devm_kzalloc(dev, sizeof(*haptics), GFP_KERNEL);
	if (!haptics)
		return -ENOMEM;
	haptics->dev = dev;

	if (!client->irq) {
		dev_err(dev, "No IRQ configured\n");
		return -EINVAL;
	}

	/* Handle DT data if provided */
	if (client->dev.of_node)
		da7280_of_to_pdata(&client->dev, haptics);

	if (haptics->op_mode == DA7280_PWM_MODE) {
		/* Get pwm and regulatot for haptics device */
		haptics->pwm_dev = devm_pwm_get(&client->dev, NULL);
		if (IS_ERR(haptics->pwm_dev)) {
			dev_err(dev, "failed to get PWM device\n");
			return PTR_ERR(haptics->pwm_dev);
		}

		/*
		 * FIXME: pwm_apply_args() should be removed when switching to
		 * the atomic PWM API.
		 */
		pwm_apply_args(haptics->pwm_dev);

		/* Check PWM Period, it must be in 10k ~ 250kHz */
		period2freq = 1000000 / pwm_get_period(haptics->pwm_dev);
		if (period2freq < DA7280_MIN_PWM_FREQ_KHZ ||
		    period2freq > DA7280_MAX_PWM_FREQ_KHZ) {
			dev_err(dev, "Not supported PWM frequency(%d)\n",
				period2freq);
			return -EINVAL;
		}
	}

	INIT_WORK(&haptics->work, da7280_haptic_work);
	haptics->client = client;
	i2c_set_clientdata(client, haptics);

	haptics->regmap =
		devm_regmap_init_i2c(client, &da7280_haptic_regmap_config);
	if (IS_ERR(haptics->regmap)) {
		ret = PTR_ERR(haptics->regmap);
		dev_err(dev, "Failed to allocate register map : %d\n",
			ret);
		return ret;
	}

	ret = devm_request_threaded_irq(dev, client->irq, NULL,
					da7280_irq_handler,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					"da7280-haptics", haptics);
	if (ret != 0) {
		dev_err(dev,
			"Failed to request IRQ : %d\n", client->irq);
		return ret;
	}

	ret = da7280_init(haptics);
	if (ret) {
		dev_err(dev, "failed to initialize device\n");
		return ret;
	}

	/* Initialize input device for haptic device */
	haptics->input_dev = devm_input_allocate_device(dev);
	if (!haptics->input_dev) {
		dev_err(dev, "failed to allocate input device\n");
		return -ENOMEM;
	}

	haptics->input_dev->name = "da7280-haptic";
	haptics->input_dev->dev.parent = client->dev.parent;
	haptics->input_dev->open = da7280_haptic_open;
	haptics->input_dev->close = da7280_haptic_close;
	input_set_drvdata(haptics->input_dev, haptics);
	input_set_capability(haptics->input_dev, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(haptics->input_dev, NULL,
				      da7280_haptic_play);
	if (ret) {
		dev_err(dev, "failed to create force-feedback\n");
		return ret;
	}

#ifdef CONFIG_SYSFS
	haptics->input_dev->dev.groups = da7280_attr_groups;
#endif

	ret = input_register_device(haptics->input_dev);
	if (ret)
		dev_err(dev, "failed to register input device\n");

	return ret;
}

static int __maybe_unused da7280_suspend(struct device *dev)
{
	struct da7280_haptic *haptics = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&haptics->input_dev->mutex);
	if (haptics->suspend_state == 0) {
		ret = regmap_update_bits(haptics->regmap,
					 DA7280_TOP_CTL1,
					 DA7280_STANDBY_EN_MASK, 0);
		if (ret)
			dev_err(haptics->dev,
				"I2C error : %d\n", ret);
		else
			haptics->suspend_state = 1;
	}
	mutex_unlock(&haptics->input_dev->mutex);
	return ret;
}

static int __maybe_unused da7280_resume(struct device *dev)
{
	struct da7280_haptic *haptics = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&haptics->input_dev->mutex);
	if (haptics->suspend_state) {
		ret = regmap_update_bits(haptics->regmap,
					 DA7280_TOP_CTL1,
					 DA7280_STANDBY_EN_MASK,
					 DA7280_STANDBY_EN_MASK);
		if (ret)
			dev_err(haptics->dev,
				"i2c error : %d\n", ret);
		else
			haptics->suspend_state = 0;
	}
	mutex_unlock(&haptics->input_dev->mutex);
	return ret;
}

static const struct of_device_id da7280_of_match[] = {
	{ .compatible = "dlg,da7280", },
	{ }
};
MODULE_DEVICE_TABLE(of, da7280_of_match);

static const struct i2c_device_id da7280_i2c_id[] = {
	{ "da7280", },
	{ }
};
MODULE_DEVICE_TABLE(i2c, da7280_i2c_id);

static SIMPLE_DEV_PM_OPS(da7280_pm_ops,
		 da7280_suspend, da7280_resume);

static struct i2c_driver da7280_driver = {
	.driver		= {
		.name	= "da7280",
		.of_match_table = of_match_ptr(da7280_of_match),
		.pm	= &da7280_pm_ops,
	},
	.probe	= da7280_probe,
	.id_table	= da7280_i2c_id,
};
module_i2c_driver(da7280_driver);

MODULE_DESCRIPTION("DA7280 haptics driver");
MODULE_AUTHOR("Roy Im <Roy.Im.Opensource@diasemi.com>");
MODULE_LICENSE("GPL");
