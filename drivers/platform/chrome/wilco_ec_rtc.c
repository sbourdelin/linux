// SPDX-License-Identifier: GPL-2.0
/*
 * wilco_ec_rtc - RTC interface for Wilco Embedded Controller
 *
 * Copyright 2018 Google LLC
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/bcd.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/timekeeping.h>
#include "wilco_ec.h"

#define EC_COMMAND_CMOS			0x7c
#define  EC_CMOS_TOD_WRITE		0x02
#define  EC_CMOS_TOD_READ		0x08

/**
 * struct ec_rtc_read - Format of RTC returned by EC.
 * @second: Second value (0..59)
 * @minute: Minute value (0..59)
 * @hour: Hour value (0..23)
 * @day: Day value (1..31)
 * @month: Month value (1..12)
 * @year: Year value (full year % 100)
 * @century: Century value (full year / 100)
 *
 * All values are presented in binary (not BCD).
 */
struct ec_rtc_read {
	u8 second;
	u8 minute;
	u8 hour;
	u8 day;
	u8 month;
	u8 year;
	u8 century;
} __packed;

/**
 * struct ec_rtc_write - Format of RTC sent to the EC.
 * @param: EC_CMOS_TOD_WRITE
 * @century: Century value (full year / 100)
 * @year: Year value (full year % 100)
 * @month: Month value (1..12)
 * @day: Day value (1..31)
 * @hour: Hour value (0..23)
 * @minute: Minute value (0..59)
 * @second: Second value (0..59)
 * @weekday: Day of the week (0=Saturday)
 *
 * All values are presented in BCD.
 */
struct ec_rtc_write {
	u8 param;
	u8 century;
	u8 year;
	u8 month;
	u8 day;
	u8 hour;
	u8 minute;
	u8 second;
	u8 weekday;
} __packed;

int wilco_ec_rtc_read(struct device *dev, struct rtc_time *tm)
{
	struct wilco_ec_device *ec = dev_get_drvdata(dev);
	u8 param = EC_CMOS_TOD_READ;
	struct ec_rtc_read rtc;
	struct wilco_ec_message msg = {
		.type = WILCO_EC_MSG_LEGACY,
		.flags = WILCO_EC_FLAG_RAW_RESPONSE,
		.command = EC_COMMAND_CMOS,
		.request_data = &param,
		.request_size = sizeof(param),
		.response_data = &rtc,
		.response_size = sizeof(rtc),
	};
	struct rtc_time calc_tm;
	unsigned long time;
	int ret;

	ret = wilco_ec_mailbox(ec, &msg);
	if (ret < 0) {
		dev_err(dev, "Failed to read EC RTC\n");
		return ret;
	}

	tm->tm_sec	= rtc.second;
	tm->tm_min	= rtc.minute;
	tm->tm_hour	= rtc.hour;
	tm->tm_mday	= rtc.day;
	/*
	 * The RTC stores the month value as 1-12 but the kernel expects 0-11,
	 * so ensure invalid/zero month value from RTC is not converted to -1.
	 */
	tm->tm_mon	= rtc.month ? rtc.month - 1 : 0;
	tm->tm_year	= rtc.year + (rtc.century * 100) - 1900;
	tm->tm_yday	= rtc_year_days(tm->tm_mday, tm->tm_mon, tm->tm_year);

	/* Compute day of week */
	rtc_tm_to_time(tm, &time);
	rtc_time_to_tm(time, &calc_tm);
	tm->tm_wday = calc_tm.tm_wday;

	return 0;
}

int wilco_ec_rtc_write(struct device *dev, struct rtc_time *tm)
{
	struct wilco_ec_device *ec = dev_get_drvdata(dev);
	struct ec_rtc_write rtc;
	struct wilco_ec_message msg = {
		.type = WILCO_EC_MSG_LEGACY,
		.flags = WILCO_EC_FLAG_RAW_RESPONSE,
		.command = EC_COMMAND_CMOS,
		.request_data = &rtc,
		.request_size = sizeof(rtc),
	};
	int year = tm->tm_year + 1900;
	/* Convert from 0=Sunday to 0=Saturday for the EC */
	int wday = tm->tm_wday == 6 ? 0 : tm->tm_wday + 1;
	int ret;

	rtc.param	= EC_CMOS_TOD_WRITE;
	rtc.century	= bin2bcd(year / 100);
	rtc.year	= bin2bcd(year % 100);
	rtc.month	= bin2bcd(tm->tm_mon + 1);
	rtc.day		= bin2bcd(tm->tm_mday);
	rtc.hour	= bin2bcd(tm->tm_hour);
	rtc.minute	= bin2bcd(tm->tm_min);
	rtc.second	= bin2bcd(tm->tm_sec);
	rtc.weekday	= bin2bcd(wday);

	ret = wilco_ec_mailbox(ec, &msg);
	if (ret < 0) {
		dev_err(dev, "Failed to write EC RTC\n");
		return ret;
	}

	return 0;
}

int wilco_ec_rtc_sync(struct device *dev)
{
	struct rtc_time tm;

	rtc_time64_to_tm(ktime_get_real_seconds(), &tm);

	return wilco_ec_rtc_write(dev, &tm);
}
