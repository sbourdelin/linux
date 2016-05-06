/* drivers/input/touchscreen/sis_i2c.c
 *  - I2C Touch panel driver for SiS 9200 family
 *
 * Copyright (C) 2011 SiS, Inc.
 * Copyright (C) 2015 Nextfour Group
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/linkage.h>
#include <linux/slab.h>
#include <linux/of_gpio.h>
#include <linux/uaccess.h>
#include <linux/irq.h>
#include <asm/unaligned.h>
#include <linux/crc-itu-t.h>

#define SIS_I2C_NAME "sis_i2c_ts"
#define MAX_FINGERS					10

#define SIS_MAX_X					4095
#define SIS_MAX_Y					4095

#define PACKET_BUFFER_SIZE				128

#define SIS_CMD_NORMAL					0x0

#define TOUCHDOWN                                       0x3
#define TOUCHUP                                         0x0
#define MAX_BYTE					 64
#define PRESSURE_MAX                                    255

/* Resolution diagonal */
#define AREA_LENGTH_LONGER				5792
/*((SIS_MAX_X^2) + (SIS_MAX_Y^2))^0.5*/
#define AREA_LENGTH_SHORT				5792
#define AREA_UNIT					(5792/32)

#define P_BYTECOUNT					0
#define ALL_IN_ONE_PACKAGE				0x10
#define IS_TOUCH(x)					((x) & 0x1)
#define IS_HIDI2C(x)					(((x) & 0xF) == 0x06)
#define IS_AREA(x)					(((x) >> 4) & 0x1)
#define IS_PRESSURE(x)				        (((x) >> 5) & 0x1)
#define IS_SCANTIME(x)			                (((x) >> 6) & 0x1)

#define NORMAL_LEN_PER_POINT			        6
#define AREA_LEN_PER_POINT				2
#define PRESSURE_LEN_PER_POINT			        1

#define TOUCH_FORMAT					0x1
#define HIDI2C_FORMAT					0x6
#define P_REPORT_ID					2
#define BYTE_BYTECOUNT					2
#define BYTE_REPORTID					1
#define BYTE_CRC_HIDI2C                                 0
#define BYTE_CRC_I2C					2
#define BYTE_SCANTIME					2

struct _touchpoint {
	u8 id;
	u16 x, y;
	u16 pressure;
	u16 width;
	u16 height;
};

struct sistp_driver_data {
	int fingers;
	struct _touchpoint pt[MAX_FINGERS];
};

struct sis_ts_data {
	struct gpio_desc *irq_gpiod;
	struct gpio_desc *reset_gpiod;
	struct i2c_client *client;
	struct input_dev *input_dev;
struct sistp_driver_data tpinfo;
};

static int sis_cul_unit(u8 report_id)
{
	int ret = NORMAL_LEN_PER_POINT;

	if (report_id != ALL_IN_ONE_PACKAGE) {

		if (IS_AREA(report_id) /*&& IS_TOUCH(report_id)*/)
			ret += AREA_LEN_PER_POINT;

		if (IS_PRESSURE(report_id))
			ret += PRESSURE_LEN_PER_POINT;
	}

	return ret;
}

static int sis_readpacket(struct i2c_client *client, u8 cmd, u8 *buf)
{
	u8 tmpbuf[MAX_BYTE] = {0};
	int ret = -1;
	int touchnum = 0;
	int p_count = 0;
	int touch_format_id = 0;
	int location = 0;
	bool read_first = true;

/*
 * I2C touch report format
 *
 * The controller sends one or two
 * 64 byte reports (depending on how many
 * contacts down etc). We read first 64 bytes
 * and then the second chunk if needed.
 * The packets are individually CRC
 * checksummed.
 *
 * buf[0] = Low 8 bits of byte count value
 * buf[1] = High 8 bits of byte counte value
 * buf[2] = Report ID
 * buf[touch num * 6 + 2 ] = Touch information
 * 1 touch point has 6 bytes, it could be none if no touch
 * buf[touch num * 6 + 3] = Touch numbers
 *
 * One touch point information include 6 bytes, the order is
 *
 * 1. status = touch down or touch up
 * 2. id = finger id
 * 3. x axis low 8 bits
 * 4. x axis high 8 bits
 * 5. y axis low 8 bits
 * 6. y axis high 8 bits
 */
	do {
		if (location >= PACKET_BUFFER_SIZE) {
			dev_err(&client->dev, "sis_readpacket: buffer overflow\n");
			return -1;
		}

		ret = i2c_master_recv(client, tmpbuf, MAX_BYTE);

		if (ret <= 0) {
			return touchnum;
		} else if (tmpbuf[P_BYTECOUNT] > MAX_BYTE) {
			dev_err(&client->dev, "sis_readpacket: invalid bytecout\n");
			return -1;
		}

		if (tmpbuf[P_BYTECOUNT] < 10)
			return touchnum;

		if (read_first)
			if (tmpbuf[P_BYTECOUNT] == 0)
				return 0;	/* touchnum is 0 */

		touch_format_id = tmpbuf[P_REPORT_ID] & 0xf;

		if ((touch_format_id != TOUCH_FORMAT)
			&& (touch_format_id != HIDI2C_FORMAT)
			&& (tmpbuf[P_REPORT_ID] != ALL_IN_ONE_PACKAGE)) {
			dev_err(&client->dev, "sis_readpacket: invalid reportid\n");
			return -1;
		}

		p_count = (int) tmpbuf[P_BYTECOUNT] - 1; /* start from 0 */
		if (tmpbuf[P_REPORT_ID] != ALL_IN_ONE_PACKAGE) {
			if (IS_TOUCH(tmpbuf[P_REPORT_ID])) {
				p_count -= BYTE_CRC_I2C; /* delete 2 byte crc */
			} else if (IS_HIDI2C(tmpbuf[P_REPORT_ID])) {
				p_count -= BYTE_CRC_HIDI2C;
			} else {
				dev_err(&client->dev, "sis_readpacket: delete crc error\n");
				return -1;
			}
			if (IS_SCANTIME(tmpbuf[P_REPORT_ID]))
				p_count -= BYTE_SCANTIME;
		}

		if (read_first)
			touchnum = tmpbuf[p_count];
		else {
			if (tmpbuf[p_count] != 0) {
				dev_err(&client->dev, "sis_readpacket: nonzero point count in tail packet\n");
				return -1;
			}
		}

		if ((touch_format_id != HIDI2C_FORMAT) &&
			(tmpbuf[P_BYTECOUNT] > 3)) {
			int crc_end = p_count +
				(IS_SCANTIME(tmpbuf[P_REPORT_ID]) * 2);
			u16 buf_crc =
				crc_itu_t(0, tmpbuf + 2, crc_end - 1);
			int l_package_crc =
				(IS_SCANTIME(tmpbuf[P_REPORT_ID]) * 2) +
				p_count + 1;
			u16 package_crc =
				get_unaligned_le16(&tmpbuf[l_package_crc]);
			if (buf_crc != package_crc) {
				dev_err(&client->dev, "sis_readpacket: CRC Error\n");
				return -1;
			}
		}

		memcpy(&buf[location], &tmpbuf[0], 64);
		/* Buf_Data [0~63] [64~128] */
		location += MAX_BYTE;
		read_first = false;
	} while (tmpbuf[P_REPORT_ID] != ALL_IN_ONE_PACKAGE &&
		tmpbuf[p_count] > 5);

	return touchnum;
}

static irqreturn_t sis_ts_irq_handler(int irq, void *dev_id)
{
	struct sis_ts_data *ts = dev_id;
	struct sistp_driver_data *tpinfo = &ts->tpinfo;

	int ret = -1;
	int point_unit;
	u8 buf[PACKET_BUFFER_SIZE] = {0};
	u8 i = 0, fingers = 0;
	u8 px = 0, py = 0, pstatus = 0;
	u8 p_area = 0;
	u8 p_preasure = 0;
	int slot;

redo:
	/* I2C or SMBUS block data read */
	ret = sis_readpacket(ts->client, SIS_CMD_NORMAL, buf);

	if (ret < 0)
		goto recheck_irq;

	else if (ret == 0) {
		fingers = 0;
		goto label_sync_input;
	}

	point_unit = sis_cul_unit(buf[P_REPORT_ID]);
	fingers = ret;

	tpinfo->fingers = fingers = (fingers > MAX_FINGERS ? 0 : fingers);

	for (i = 0; i < fingers; i++) {
		if ((buf[P_REPORT_ID] != ALL_IN_ONE_PACKAGE) && (i >= 5)) {
			pstatus = BYTE_BYTECOUNT + BYTE_REPORTID
				+ ((i - 5) * point_unit);
			pstatus += 64;
		} else {
			pstatus = BYTE_BYTECOUNT + BYTE_REPORTID
				+ (i * point_unit);
		}
		/* X and Y coordinate locations */
		px = pstatus + 2;
		py = px + 2;

		if ((buf[pstatus]) == TOUCHUP) {
			tpinfo->pt[i].width    = 0;
			tpinfo->pt[i].height   = 0;
			tpinfo->pt[i].pressure = 0;
		} else if (buf[P_REPORT_ID] == ALL_IN_ONE_PACKAGE
			&& (buf[pstatus]) == TOUCHDOWN) {
			tpinfo->pt[i].width    = 1;
			tpinfo->pt[i].height   = 1;
			tpinfo->pt[i].pressure = 1;
		} else if ((buf[pstatus]) == TOUCHDOWN) {
			p_area = py + 2;
			p_preasure = py + 2 + (IS_AREA(buf[P_REPORT_ID]) * 2);

			if (IS_AREA(buf[P_REPORT_ID])) {
				tpinfo->pt[i].width = buf[p_area];
				tpinfo->pt[i].height = buf[p_area + 1];
			} else {
				tpinfo->pt[i].width = 1;
				tpinfo->pt[i].height = 1;
			}

			if (IS_PRESSURE(buf[P_REPORT_ID]))
				tpinfo->pt[i].pressure = (buf[p_preasure]);
			else
				tpinfo->pt[i].pressure = 1;
		} else {
			dev_err(&ts->client->dev, "Touch status error\n");
			goto recheck_irq;
		}
		tpinfo->pt[i].id = (buf[pstatus + 1]);
		tpinfo->pt[i].x = le16_to_cpu(get_unaligned_le16(&buf[px]));
		tpinfo->pt[i].y = le16_to_cpu(get_unaligned_le16(&buf[py]));

		slot = input_mt_get_slot_by_key(
			ts->input_dev, tpinfo->pt[i].id);

		if (slot < 0)
			continue;

		input_mt_slot(ts->input_dev, slot);
		input_mt_report_slot_state(ts->input_dev,
					MT_TOOL_FINGER, tpinfo->pt[i].pressure);

		if (tpinfo->pt[i].pressure) {

			tpinfo->pt[i].width *= AREA_UNIT;
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR,
					tpinfo->pt[i].width);
			tpinfo->pt[i].height *= AREA_UNIT;
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MINOR,
					tpinfo->pt[i].height);
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE,
					tpinfo->pt[i].pressure);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_X,
					tpinfo->pt[i].x);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,
					tpinfo->pt[i].y);
		}
	}

label_sync_input:
	input_mt_sync_frame(ts->input_dev);
	input_sync(ts->input_dev);

recheck_irq:
	if (ts->irq_gpiod) {
		/*
		 * If provided and interrupt gpio and
		 * irq is still asserted,
		 * read data until interrupt is deasserted.
		 */
		ret = gpiod_get_value_cansleep(ts->irq_gpiod);
		if (ret == 1)
			goto redo;
	}

	return IRQ_HANDLED;
}

static void sis_ts_reset(struct i2c_client *client, struct sis_ts_data *ts)
{

	ts->irq_gpiod   = devm_gpiod_get_optional(&client->dev,
						"irq", GPIOD_IN);
	ts->reset_gpiod = devm_gpiod_get_optional(&client->dev,
						"reset", GPIOD_OUT_LOW);

	if (ts->reset_gpiod) {
		/* Get out of reset */
		msleep(1);
		gpiod_set_value(ts->reset_gpiod, 1);
		msleep(1);
		gpiod_set_value(ts->reset_gpiod, 0);
		msleep(100);
	}
}

static int sis_ts_probe(
	struct i2c_client *client, const struct i2c_device_id *id)
{
	int error = 0;
	struct sis_ts_data *ts = NULL;

	ts = devm_kzalloc(&client->dev, sizeof(struct sis_ts_data), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	sis_ts_reset(client, ts);

	ts->client = client;
	i2c_set_clientdata(client, ts);

	ts->input_dev = devm_input_allocate_device(&client->dev);
	if (!ts->input_dev) {
		dev_err(&client->dev, "sis_ts_probe: Failed to allocate input device\n");
		return -ENOMEM;
	}

	ts->input_dev->name = "sis_touch";
	ts->input_dev->id.bustype = BUS_I2C;

	input_set_abs_params(ts->input_dev, ABS_MT_PRESSURE,
			0, PRESSURE_MAX, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR,
			0, AREA_LENGTH_LONGER, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MINOR,
			0, AREA_LENGTH_SHORT, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X,
			0, SIS_MAX_X, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y,
			0, SIS_MAX_Y, 0, 0);

	error = input_mt_init_slots(ts->input_dev, MAX_FINGERS,
			INPUT_MT_DROP_UNUSED | INPUT_MT_DIRECT);

	if (error) {
		dev_err(&client->dev,
			"failed to initialize MT slots: %d\n", error);
		return error;
	}

	error = input_register_device(ts->input_dev);
	if (error) {
		dev_err(&client->dev,
			"unable to register input device: %d\n", error);
		return error;
	}

	error = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					sis_ts_irq_handler,
					IRQF_ONESHOT,
					client->name, ts);

	if (error) {
		dev_err(&client->dev, "request irq failed\n");
		return error;
	}

	return 0;
}

static const struct i2c_device_id sis_ts_id[] = {
	{ SIS_I2C_NAME, 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, sis_ts_id);

#ifdef CONFIG_OF
static const struct of_device_id sis_ts_dt_ids[] = {
	{ .compatible = "sis,9200_ts" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sis_ts_dt_ids);
#endif

static struct i2c_driver sis_ts_driver = {
	.probe		= sis_ts_probe,
	.id_table	= sis_ts_id,
	.driver = {
		.name	= SIS_I2C_NAME,
		.of_match_table = of_match_ptr(sis_ts_dt_ids),
	},
};

module_i2c_driver(sis_ts_driver);
MODULE_DESCRIPTION("SiS 9200 Family Touchscreen Driver");
MODULE_LICENSE("GPL v2");
