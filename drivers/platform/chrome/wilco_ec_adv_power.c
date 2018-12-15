// SPDX-License-Identifier: GPL-2.0
/*
 * wilco_ec_adv_power - peakshift and adv_batt_charging config of Wilco EC
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

#include <linux/kobject.h>
#include <linux/device.h>
#include "wilco_ec_adv_power.h"
#include "wilco_ec_properties.h"
#include "wilco_ec_sysfs_util.h"
#include "wilco_ec.h"

struct adv_batt_charging_data {
	int duration_hours;
	int duration_minutes;
	int start_hours;
	int start_minutes;
};

struct peakshift_data {
	int start_hours;
	int start_minutes;
	int end_hours;
	int end_minutes;
	int charge_start_hours;
	int charge_start_minutes;
};

/**
 * struct time_bcd_format - spec for binary coded decimal time format
 * @hour_position: how many bits left within the byte is the hour
 * @minute_position: how many bits left within the byte is the minute
 *
 * Date and hour information is passed to/from the EC using packed bytes,
 * where each byte represents an hour and a minute that some event occurs.
 * The minute field always happens at quarter-hour intervals, so either
 * 0, 15, 20, or 45. This allows this info to be packed within 2 bits.
 * Along with the 5 bits of hour info [0-23], this gives us 7 used bits
 * within each packed byte. The annoying thing is that the PEAKSHIFT and
 * ADVANCED_BATTERY_CHARGING properties pack these 7 bits differently,
 * hence this struct.
 */
struct time_bcd_format {
	u8 hour_position;
	u8 minute_position;
};

const struct time_bcd_format PEAKSHIFT_BCD_FORMAT = {
			     // bit[0] is unused
	.hour_position = 1,  // bits[1:7]
	.minute_position = 6 // bits[6:8]
};

const struct time_bcd_format ADV_BATT_CHARGING_BCD_FORMAT = {
	.minute_position = 0, // bits[0:2]
	.hour_position = 2    // bits[2:7]
			      // bit[7] is unused
};

/**
 * struct peakshift_payload - The formatted peakshift time sent/received by EC.
 * @start_time: packed byte of hour and minute info
 * @end_time: packed byte of hour and minute info
 * @charge_start_time: packed byte of hour and minute info
 * @RESERVED: an unused padding byte
 */
struct peakshift_payload {
	u8 start_time;
	u8 end_time;
	u8 charge_start_time;
	u8 RESERVED;
} __packed;

struct adv_batt_charging_payload {
	u16 RESERVED;
	u8 duration_time;
	u8 start_time;
} __packed;

/**
 * extract_quarter_hour() - Convert from literal minutes to quarter hour.
 * @minutes: Literal minutes value. Needs to be one of {0, 15, 30, 45}
 *
 * Return one of {0, 1, 2, 3} for each of {0, 15, 30, 45}, or -EINVAL on error.
 */
static int extract_quarter_hour(int minutes)
{
	if ((minutes < 0) || (minutes > 45) || minutes % 15)
		return -EINVAL;
	return minutes / 15;
}

static int check_adv_batt_charging_data(struct device *dev,
					struct adv_batt_charging_data *data)
{
	if (data->start_hours < 0 || data->start_hours > 23) {
		dev_err(dev, "start_hours must be in [0-23], got %d",
			data->start_hours);
		return -EINVAL;
	}
	if (data->duration_hours < 0 || data->duration_hours > 23) {
		dev_err(dev, "duration_hours must be in [0-23], got %d",
			data->duration_hours);
		return -EINVAL;
	}
	if (data->start_minutes < 0 || data->start_minutes > 59) {
		dev_err(dev, "start_minutes must be in [0-59], got %d",
			data->start_minutes);
		return -EINVAL;
	}
	if (data->duration_minutes < 0 || data->duration_minutes > 59) {
		dev_err(dev, "duration_minutes must be in [0-59], got %d",
			data->duration_minutes);
		return -EINVAL;
	}
	return 0;
}

static int check_peakshift_data(struct device *dev, struct peakshift_data *data)
{
	if (data->start_hours < 0 || data->start_hours > 23) {
		dev_err(dev, "start_hours must be in [0-23], got %d",
			data->start_hours);
		return -EINVAL;
	}
	if (data->end_hours < 0 || data->end_hours > 23) {
		dev_err(dev, "end_hours must be in [0-23], got %d",
			data->end_hours);
		return -EINVAL;
	}
	if (data->charge_start_hours < 0 || data->charge_start_hours > 23) {
		dev_err(dev, "charge_start_hours must be in [0-23], got %d",
			data->charge_start_hours);
		return -EINVAL;
	}
	if (data->start_minutes < 0 || data->start_minutes > 59) {
		dev_err(dev, "start_minutes must be in [0-59], got %d",
			data->start_minutes);
		return -EINVAL;
	}
	if (data->end_minutes < 0 || data->end_minutes > 59) {
		dev_err(dev, "end_minutes must be in [0-59], got %d",
			data->end_minutes);
		return -EINVAL;
	}
	if (data->charge_start_minutes < 0 || data->charge_start_minutes > 59) {
		dev_err(dev, "charge_start_minutes must be in [0-59], got %d",
			data->charge_start_minutes);
		return -EINVAL;
	}
	return 0;
}

/**
 * pack_field() - Pack hour and minute info into a byte.
 *
 * @fmt: The format for how to place the info within the byte
 * @hours: In range [0-23]
 * @quarter_hour: In range [0-3], representing :00, :15, :30, and :45
 *
 * Return the packed byte.
 */
static u8 pack_field(struct time_bcd_format fmt, int hours, int quarter_hour)
{
	int result = 0;

	result |= hours << fmt.hour_position;
	result |= quarter_hour << fmt.minute_position;
	return (u8) result;
}

/**
 * unpack_field() - Extract hour and minute info from a byte.
 *
 * @fmt: The format for how to place the info within the byte
 * @field: Byte which contains the packed info
 * @hours: The value to be filled, in [0, 24]
 * @quarter_hour: to be filled in range [0-3], meaning :00, :15, :30, and :45
 */
static void unpack_field(struct time_bcd_format fmt, u8 field, int *hours,
			 int *quarter_hour)
{
	*hours =	(field >> fmt.hour_position)   & 0x1f; // 00011111
	*quarter_hour = (field >> fmt.minute_position) & 0x03; // 00000011
}

static void pack_adv_batt_charging(struct adv_batt_charging_data *data,
		     struct adv_batt_charging_payload *payload)
{
	payload->start_time = pack_field(ADV_BATT_CHARGING_BCD_FORMAT,
					 data->start_hours,
					 data->start_minutes);
	payload->duration_time = pack_field(ADV_BATT_CHARGING_BCD_FORMAT,
				       data->duration_hours,
				       data->duration_minutes);
}

static void unpack_adv_batt_charging(struct adv_batt_charging_data *data,
		       struct adv_batt_charging_payload *payload)
{
	unpack_field(ADV_BATT_CHARGING_BCD_FORMAT, payload->start_time,
		     &(data->start_hours),
		     &(data->start_minutes));
	unpack_field(ADV_BATT_CHARGING_BCD_FORMAT, payload->duration_time,
		     &(data->duration_hours),
		     &(data->duration_minutes));
}

static void pack_peakshift(struct peakshift_data *data,
			   struct peakshift_payload *payload)
{
	payload->start_time = pack_field(PEAKSHIFT_BCD_FORMAT,
					 data->start_hours,
					 data->start_minutes);
	payload->end_time = pack_field(PEAKSHIFT_BCD_FORMAT,
				       data->end_hours,
				       data->end_minutes);
	payload->charge_start_time = pack_field(PEAKSHIFT_BCD_FORMAT,
						data->charge_start_hours,
						data->charge_start_minutes);
}

static void unpack_peakshift(struct peakshift_data *data,
			     struct peakshift_payload *payload)
{
	unpack_field(PEAKSHIFT_BCD_FORMAT, payload->start_time,
		     &(data->start_hours),
		     &(data->start_minutes));
	unpack_field(PEAKSHIFT_BCD_FORMAT, payload->end_time,
		     &(data->end_hours),
		     &(data->end_minutes));
	unpack_field(PEAKSHIFT_BCD_FORMAT, payload->charge_start_time,
		     &(data->charge_start_hours),
		     &(data->charge_start_minutes));
}

ssize_t wilco_ec_peakshift_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct property_attribute *prop_attr;
	struct device *dev;
	struct wilco_ec_device *ec;
	struct peakshift_payload payload;
	struct peakshift_data data;
	const char FORMAT[] = "%02d %02d %02d %02d %02d %02d\n";
	const int OUT_LENGTH = 18; //six 2-char nums, 5 spaces, 1 newline
	int ret;

	if (OUT_LENGTH + 1 > PAGE_SIZE)
		return -ENOBUFS; //no buffer space for message + null

	prop_attr = container_of(attr, struct property_attribute, kobj_attr);
	dev = device_from_kobject(kobj);
	ec = dev_get_drvdata(dev);

	/* get the raw payload of data from the EC */
	ret = wilco_ec_get_property(ec, prop_attr->pid, sizeof(payload),
				    (u8 *) &payload);
	if (ret < 0) {
		dev_err(dev, "error in wilco_ec_mailbox()");
		return ret;
	}

	/* unpack raw bytes, and convert quarter-hour to literal minute */
	unpack_peakshift(&data, &payload);
	data.start_minutes *= 15;
	data.end_minutes *= 15;
	data.charge_start_minutes *= 15;

	/* Check that the EC returns good data */
	ret = check_peakshift_data(dev, &data);
	if (ret < 0) {
		dev_err(dev, "EC returned out of range minutes or hours");
		return -EBADMSG;
	}

	/* Print the numbers to the string */
	ret = scnprintf(buf, OUT_LENGTH+1, FORMAT,
			data.start_hours,
			data.start_minutes,
			data.end_hours,
			data.end_minutes,
			data.charge_start_hours,
			data.charge_start_minutes);
	if (ret != OUT_LENGTH) {
		dev_err(dev, "expected to write %d chars, wrote %d", OUT_LENGTH,
			ret);
		return -EIO;
	}

	return OUT_LENGTH;
}

ssize_t wilco_ec_peakshift_store(struct kobject *kobj,
				 struct kobj_attribute *attr, const char *buf,
				 size_t count)
{
	struct property_attribute *prop_attr;
	struct device *dev;
	struct wilco_ec_device *ec;
	struct peakshift_data data;
	struct peakshift_payload payload;
	const char FORMAT[] = "%d %d %d %d %d %d";
	int ret;

	prop_attr = container_of(attr, struct property_attribute, kobj_attr);
	dev = device_from_kobject(kobj);
	ec = dev_get_drvdata(dev);

	/* Extract our 6 numbers from the input string */
	ret = sscanf(buf, FORMAT,
		     &data.start_hours,
		     &data.start_minutes,
		     &data.end_hours,
		     &data.end_minutes,
		     &data.charge_start_hours,
		     &data.charge_start_minutes);
	if (ret != 6) {
		dev_err(dev, "unable to parse '%s' into 6 integers", buf);
		return -EINVAL;
	}

	/* Ensure the integers we parsed are valid */
	ret = check_peakshift_data(dev, &data);
	if (ret < 0)
		return ret;

	/* Convert the literal minutes to which quarter hour they represent */
	data.start_minutes = extract_quarter_hour(data.start_minutes);
	if (data.start_minutes < 0)
		goto bad_minutes;
	data.end_minutes = extract_quarter_hour(data.end_minutes);
	if (data.end_minutes < 0)
		goto bad_minutes;
	data.charge_start_minutes = extract_quarter_hour(
						data.charge_start_minutes);
	if (data.charge_start_minutes < 0)
		goto bad_minutes;

	/* Create the raw byte payload and send it off */
	pack_peakshift(&data, &payload);
	wilco_ec_set_property(ec, OP_SET, prop_attr->pid, sizeof(payload),
			      (u8 *) &payload);

	return count;

bad_minutes:
	dev_err(dev, "minutes must be at the quarter hour");
	return -EINVAL;
}

ssize_t wilco_ec_peakshift_batt_thresh_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	struct device *dev;
	struct wilco_ec_device *ec;
	const char FORMAT[] = "%02d\n";
	size_t RESULT_LENGTH = 3; /* 2-char number and newline */
	u8 percent;
	int ret;

	dev = device_from_kobject(kobj);
	ec = dev_get_drvdata(dev);

	ret = wilco_ec_get_property(ec, PID_PEAKSHIFT_BATTERY_THRESHOLD, 1,
				    &percent);
	if (ret < 0)
		return ret;

	if (percent < 15 || percent > 50) {
		dev_err(ec->dev, "expected 15 < percentage < 50, got %d",
			percent);
		return -EBADMSG;
	}

	scnprintf(buf, RESULT_LENGTH+1, FORMAT, percent);

	return RESULT_LENGTH;
}

ssize_t wilco_ec_peakshift_batt_thresh_store(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	struct device *dev;
	struct wilco_ec_device *ec;
	u8 DECIMAL_BASE = 10;
	u8 percent;
	int ret;

	dev = device_from_kobject(kobj);
	ec = dev_get_drvdata(dev);


	ret = kstrtou8(buf, DECIMAL_BASE, &percent);
	if (ret) {
		dev_err(dev, "unable to parse '%s' to u8", buf);
		return ret;
	}

	if (percent < 15 || percent > 50) {
		dev_err(dev, "require 15 < batt_thresh_percent < 50, got %d",
			percent);
		return -EINVAL;
	}

	ret = wilco_ec_set_property(ec, OP_SET, PID_PEAKSHIFT_BATTERY_THRESHOLD,
				    1, &percent);
	if (ret < 0)
		return ret;

	return count;
}

ssize_t wilco_ec_abc_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	struct property_attribute *prop_attr;
	struct device *dev;
	struct wilco_ec_device *ec;
	struct adv_batt_charging_payload payload;
	struct adv_batt_charging_data data;
	const char FORMAT[] = "%02d %02d %02d %02d\n";
	const int OUT_LENGTH = 12; //four 2-char nums, 3 spaces, 1 newline
	int ret;

	prop_attr = container_of(attr, struct property_attribute, kobj_attr);
	dev = device_from_kobject(kobj);
	ec = dev_get_drvdata(dev);


	if (OUT_LENGTH + 1 > PAGE_SIZE)
		return -ENOBUFS; //no buffer space for message + null

	/* get the raw payload of data from the EC */
	ret = wilco_ec_get_property(ec, prop_attr->pid, sizeof(payload),
				    (u8 *) &payload);
	if (ret < 0) {
		dev_err(dev, "error in wilco_ec_mailbox()");
		return ret;
	}

	/* unpack raw bytes, and convert quarter-hour to literal minute */
	unpack_adv_batt_charging(&data, &payload);
	data.start_minutes *= 15;
	data.duration_minutes *= 15;

	// /* Is this needed? can we assume the EC returns good data? */
	// EC is returning 00 00 27 30. Was this modified, or is EC weird
	// out of the box?
	ret = check_adv_batt_charging_data(dev, &data);
	if (ret < 0) {
		dev_err(dev, "EC returned out of range minutes or hours");
		return -EBADMSG;
	}

	/* Print the numbers to the string */
	ret = scnprintf(buf, OUT_LENGTH+1, FORMAT,
			data.start_hours,
			data.start_minutes,
			data.duration_hours,
			data.duration_minutes);
	if (ret != OUT_LENGTH) {
		dev_err(dev, "expected to write %d chars, wrote %d", OUT_LENGTH,
			ret);
		return -EIO;
	}

	return OUT_LENGTH;
}

ssize_t wilco_ec_abc_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	struct property_attribute *prop_attr;
	struct device *dev;
	struct wilco_ec_device *ec;
	struct adv_batt_charging_data data;
	struct adv_batt_charging_payload payload;
	const char FORMAT[] = "%d %d %d %d";
	int ret;

	prop_attr = container_of(attr, struct property_attribute, kobj_attr);
	dev = device_from_kobject(kobj);
	ec = dev_get_drvdata(dev);

	/* Extract our 4 numbers from the input string */
	ret = sscanf(buf, FORMAT,
		     &data.start_hours,
		     &data.start_minutes,
		     &data.duration_hours,
		     &data.duration_minutes);
	if (ret != 4) {
		dev_err(dev, "unable to parse '%s' into 4 integers", buf);
		return -EINVAL;
	}

	/* Ensure the integers we parsed are valid */
	ret = check_adv_batt_charging_data(dev, &data);
	if (ret < 0)
		return ret;

	/* Convert the literal minutes to which quarter hour they represent */
	data.start_minutes = extract_quarter_hour(data.start_minutes);
	if (data.start_minutes < 0)
		goto bad_minutes;
	data.duration_minutes = extract_quarter_hour(data.duration_minutes);
	if (data.duration_minutes < 0)
		goto bad_minutes;

	/* Create the raw byte payload and send it off */
	pack_adv_batt_charging(&data, &payload);
	wilco_ec_set_property(ec, OP_SET, prop_attr->pid, sizeof(payload),
			      (u8 *) &payload);

	return count;

bad_minutes:
	dev_err(dev, "minutes must be at the quarter hour");
	return -EINVAL;
}
