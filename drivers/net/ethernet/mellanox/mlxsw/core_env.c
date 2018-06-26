/*
 * drivers/net/ethernet/mellanox/mlxsw/core_env.c
 * Copyright (c) 2018 Mellanox Technologies. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>

#include "core.h"
#include "core_env.h"
#include "item.h"

union mlxsw_env_port_thresh {
	u8 buf[MLXSW_REG_MCIA_TH_SIZE];
	struct mlxsw_env_port_temp_th {
		u16 temp_alarm_hi;
		u16 temp_alarm_lo;
		u16 temp_warn_hi;
		u16 temp_warn_low;
	} t;
};

static int mlxsw_env_bulk_get(struct mlxsw_core *core,
			      int *ports_temp_cache, int port_count,
			      bool *untrusted_sensor)
{
	char mtbr_pl[MLXSW_REG_MTBR_LEN];
	int i, j, count, off;
	u16 temp;
	int err;

	/* Read ports temperature. */
	if (untrusted_sensor)
		*untrusted_sensor = false;
	count = 0;
	while (count < port_count) {
		off = min_t(u8, MLXSW_REG_MTBR_REC_MAX_COUNT,
			    port_count - count);
		mlxsw_reg_mtbr_pack(mtbr_pl, MLXSW_REG_MTBR_BASE_PORT_INDEX +
				    count, off);
		err = mlxsw_reg_query(core, MLXSW_REG(mtbr), mtbr_pl);
		if (err)
			return err;

		for (i = 0, j = count; i < off; i++, j++) {
			mlxsw_reg_mtbr_temp_unpack(mtbr_pl, i, &temp, NULL);

			/* Update status and temperature cache. */
			switch (temp) {
			case MLXSW_REG_MTBR_NO_CONN:
			case MLXSW_REG_MTBR_NO_TEMP_SENS:
			case MLXSW_REG_MTBR_INDEX_NA:
				ports_temp_cache[j] = 0;
				break;
			case MLXSW_REG_MTBR_BAD_SENS_INFO:
				/* Untrusted cable is connected. It means that
				 * reading temperature from its sensor is
				 * unreliable and thermal control should
				 * consider increasing system's FAN speed
				 * according to the system requirements.
				 * The presence of untrusted cable is exposed
				 * to hwmon through temp1_fault attribute.
				 */
				ports_temp_cache[j] = 0;
				if (untrusted_sensor)
					*untrusted_sensor = false;
				break;
			default:
				ports_temp_cache[j] =
					MLXSW_REG_MTMP_TEMP_TO_MC(temp);
				break;
			}
		}
		count += off;
	}

	return 0;
}

static void mlxsw_env_scale_temp(int hot, int crit, int tdelta, u8 mask,
				 int *temp)
{
	int twindow;

	/* Scale port temperature thresholds window to the based window: do
	 * nothong, if windows are equal, shrink window if it exceeds, expand
	 * in other case. Set delta according this scale.
	 */
	twindow = crit - hot;
	if (twindow > MLXSW_ENV_TEMP_WINDOW)
		tdelta /= DIV_ROUND_CLOSEST(twindow, MLXSW_ENV_TEMP_WINDOW);
	else if (twindow < MLXSW_ENV_TEMP_WINDOW)
		tdelta *= DIV_ROUND_CLOSEST(MLXSW_ENV_TEMP_WINDOW, twindow);

	switch (mask) {
	case MLXSW_ENV_CRIT_MASK:
		*temp = clamp_val(MLXSW_ENV_TEMP_HOT + tdelta,
				  MLXSW_ENV_TEMP_HOT, MLXSW_ENV_TEMP_CRIT);
		break;
	case MLXSW_ENV_HOT_MASK:
		*temp = clamp_val(MLXSW_ENV_TEMP_NORM + tdelta,
				  MLXSW_ENV_TEMP_NORM, MLXSW_ENV_TEMP_HOT);
		break;
	default:
		/* Don't set temperature below nominal value. */
		tdelta %= MLXSW_ENV_TEMP_NORM;
		*temp = clamp_val(MLXSW_ENV_TEMP_NORM - tdelta, *temp,
				  MLXSW_ENV_TEMP_NORM);
		break;
	}
}

static void mlxsw_env_process_temp(int temp,
				   struct mlxsw_env_temp_thresh *port,
				   struct mlxsw_env_temp_thresh *delta,
				   struct mlxsw_env_temp_multi *multi)
{
	int tdelta;

	/* Compare each port temperature sensors values, with warning and
	 * threshold values for this port. Find the worst delta for the all,
	 * sensors which is defined as following:
	 * - if value is below the warning threshold - the closest value to the
	 *   warning threshold;
	 * - if value is between the warning and alarm thresholds - the closet
	 *   value to the alarm threshold;
	 * - if value is above the alarm threshold - the value with the biggest
	 *   delta.
	 * The temperature value should be set according to the worst delta
	 * with the next priority:
	 * - if any sensor above alarm threshold - from the alarm;
	 * - if any sensor above warning threshold - from the hot;
	 * - from norm in other case.
	 */
	if (!multi->mask && temp < port->hot) {
		tdelta = port->hot - temp;
		mlxsw_env_scale_temp(port->hot, port->crit, tdelta, 0, &temp);
		if (tdelta < delta->normal) {
			multi->thresh.normal = temp;
			delta->normal = tdelta;
		}
	} else if (temp >= port->crit) {
		tdelta = temp - port->crit;
		mlxsw_env_scale_temp(port->hot, port->crit, tdelta,
				     MLXSW_ENV_CRIT_MASK, &temp);
		if (tdelta > delta->crit) {
			multi->thresh.crit = temp;
			delta->crit = tdelta;
		}
		multi->mask |= MLXSW_ENV_CRIT_MASK;
	} else if (!(multi->mask & MLXSW_ENV_CRIT_MASK)) {
		tdelta = temp - port->hot;
		mlxsw_env_scale_temp(port->hot, port->crit, tdelta,
				     MLXSW_ENV_HOT_MASK, &temp);
		if (tdelta > delta->hot) {
			multi->thresh.hot = temp;
			delta->hot = tdelta;
		}
		multi->mask |= MLXSW_ENV_HOT_MASK;
	}
}

static void
mlxsw_env_finalize_temp(struct mlxsw_env_temp_thresh *delta,
			struct mlxsw_env_temp_multi *multi, int *temp)
{
	/* If the values from the all temperature sensors are:
	 * - above temperature warning threshold - pick for the temperature the
	 *   value with biggest delta between the temperature alarm threshold;
	 * - between the temperature warning threshold and the temperature
	 *   alarm threshold - pick as the temperature the closest value to the
	 *   the temperature warning threshold;
	 * - below the temperature warning threshold - pick as the temperature
	 *   the closest to the temperature warning threshold.
	 */
	if (multi->mask & MLXSW_ENV_CRIT_MASK)
		*temp = multi->thresh.crit;
	else if (multi->mask & MLXSW_ENV_HOT_MASK)
		*temp = multi->thresh.hot;
	else
		*temp = multi->thresh.normal;
}

static int mlxsw_env_validate_cable_ident(struct mlxsw_core *core, int id,
					  bool *qsfp)
{
	char eeprom_tmp[MLXSW_REG_MCIA_EEPROM_SIZE];
	char mcia_pl[MLXSW_REG_MCIA_LEN];
	u8 ident;
	int err;

	mlxsw_reg_mcia_pack(mcia_pl, id, 0, MLXSW_REG_MCIA_PAGE0_LO_OFF, 0, 1,
			    MLXSW_REG_MCIA_I2C_ADDR_LOW);
	err = mlxsw_reg_query(core, MLXSW_REG(mcia), mcia_pl);
	if (err)
		return err;
	mlxsw_reg_mcia_eeprom_memcpy_from(mcia_pl, eeprom_tmp);
	ident = eeprom_tmp[0];
	switch (ident) {
	case MLXSW_REG_MCIA_EEPROM_MODULE_INFO_ID_SFP:
		*qsfp = false;
		break;
	case MLXSW_REG_MCIA_EEPROM_MODULE_INFO_ID_QSFP:
	case MLXSW_REG_MCIA_EEPROM_MODULE_INFO_ID_QSFP_PLUS:
	case MLXSW_REG_MCIA_EEPROM_MODULE_INFO_ID_QSFP28:
	case MLXSW_REG_MCIA_EEPROM_MODULE_INFO_ID_QSFP_DD:
		*qsfp = true;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int mlxsw_env_collect_port_temp(struct mlxsw_core *core, int *ports_temp_cache,
				int port_count,
				struct mlxsw_env_temp_multi *multi,
				struct mlxsw_env_temp_thresh *delta,
				bool *untrusted_sensor, int *temp)
{
	char eeprom_tmp[MLXSW_REG_MCIA_EEPROM_SIZE];
	union mlxsw_env_port_thresh thresh;
	char mcia_pl[MLXSW_REG_MCIA_LEN];
	struct mlxsw_env_temp_thresh curr;
	int port_temp, i;
	bool qsfp;
	int err;

	memset(&curr, 0, sizeof(struct mlxsw_env_temp_thresh));
	/* Read ports temperature. */
	err = mlxsw_env_bulk_get(core, ports_temp_cache, port_count,
				 untrusted_sensor);
	if (err)
		return err;

	for (i = 0; i < port_count; i++) {
		/* Skip port with no temperature sensor */
		if (!ports_temp_cache[i])
			continue;

		/* Read Free Side Device Temperature Thresholds from page 03h
		 * (MSB at lower byte address).
		 * Bytes:
		 * 128-129 - Temp High Alarm
		 * 130-131 - Temp Low Alarm
		 * 132-133 - Temp High Warning
		 * 134-135 - Temp Low Warning
		 */

		/* Validate module identifier value. */
		err = mlxsw_env_validate_cable_ident(core, i, &qsfp);
		if (err)
			return err;

		if (qsfp)
			mlxsw_reg_mcia_pack(mcia_pl, i, 0,
					    MLXSW_REG_MCIA_TH_PAGE_NUM,
					    MLXSW_REG_MCIA_TH_PAGE_OFF,
					    MLXSW_REG_MCIA_TH_SIZE,
					    MLXSW_REG_MCIA_I2C_ADDR_LOW);
		else
			mlxsw_reg_mcia_pack(mcia_pl, i, 0,
					    MLXSW_REG_MCIA_PAGE0_LO, 0,
					    MLXSW_REG_MCIA_TH_SIZE,
					    MLXSW_REG_MCIA_I2C_ADDR_HIGH);

		err = mlxsw_reg_query(core, MLXSW_REG(mcia), mcia_pl);
		if (err)
			return err;

		mlxsw_reg_mcia_eeprom_memcpy_from(mcia_pl, eeprom_tmp);
		memcpy(thresh.buf, eeprom_tmp, MLXSW_REG_MCIA_TH_SIZE);
		/* Skip sensor with no threshold info. */
		if (!thresh.t.temp_warn_hi || !thresh.t.temp_warn_hi)
			continue;

		port_temp = ports_temp_cache[i];
		curr.hot = thresh.t.temp_warn_hi * 1000;
		curr.crit = thresh.t.temp_alarm_hi * 1000;
		mlxsw_env_process_temp(port_temp, &curr, delta, multi);
	}

	mlxsw_env_finalize_temp(delta, multi, temp);

	return 0;
}
