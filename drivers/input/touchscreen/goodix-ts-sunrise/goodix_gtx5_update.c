/*
 * Goodix GTx5 Touchscreen Driver.
 *
 * Copyright (C) 2015 - 2016 Goodix, Inc.
 * Authors:  Wang Yafei <wangyafei@goodix.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be a reference
 * to you, when you are integrating the GOODiX's CTP IC into your system,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include "goodix_ts_core.h"

/* COMMON PART - START */
#define TS_DEFAULT_FIRMWARE			"goodix_ts_fw.bin"

#define FW_HEADER_SIZE				256
#define FW_SUBSYS_INFO_SIZE			8
#define FW_SUBSYS_INFO_OFFSET			32
#define FW_SUBSYS_MAX_NUM			24
#define FW_NAME_MAX				128

#define ISP_MAX_BUFFERSIZE			(1024 * 16)

#define HW_REG_CPU_EN				0x4180
#define HW_REG_ILM_ACCESS			0x50C0
#define HW_REG_BANK_SELECT			0x50C4
#define HW_REG_ISP_ADDR				0x8000
#define HW_REG_ISP_STAT				0x4195
#define HW_REG_ISP_CMD				0x4196
#define HW_REG_ISP_PKT_INFO			0xFFF0
#define HW_REG_ISP_RESULT			0x4197
#define HW_REG_ISP_BUFFER			0x8000
#define HW_REG_BOOT_FLAG			0x434C
#define HW_REG_BOOT_CTRL0			0xF7CC
#define HW_REG_BOOT_CTRL1			0xF7EC
#define HW_REG_WDT				0x40B0

#define CPU_CTRL_PENDING			0x00
#define CPU_CTRL_RUNNING			0x01

#define ISP_STAT_IDLE				0xFF
#define ISP_STAT_READY				0xAA
#define ISP_STAT_WRITING			0xCC
#define ISP_FLASH_ERROR				0xEE
#define ISP_FLASH_SUCCESS			0xDD
#define ISP_CMD_PREPARE				0x55
#define ISP_CMD_FLASH				0xAA

/**
 * fw_subsys_info - subsytem firmware information
 * @type: sybsystem type
 * @size: firmware size
 * @flash_addr: flash address
 * @data: firmware data
 */
struct fw_subsys_info {
	u8 type;
	u32 size;
	u32 flash_addr;
	const u8 *data;
};

#pragma pack(1)
/**
 * firmware_info
 * @size: fw total length
 * @checksum: checksum of fw
 * @hw_pid: mask pid string
 * @hw_pid: mask vid code
 * @fw_pid: fw pid string
 * @fw_vid: fw vid code
 * @subsys_num: number of fw subsystem
 * @chip_type: chip type
 * @protocol_ver: firmware packing
 *   protocol version
 * @subsys: sybsystem info
 */
struct firmware_info {
	u32 size;
	u16 checksum;
	u8 hw_pid[6];
	u8 hw_vid[3];
	u8 fw_pid[8];
	u8 fw_vid[3];
	u8 subsys_num;
	u8 chip_type;
	u8 protocol_ver;
	u8 reserved[3];
	struct fw_subsys_info subsys[FW_SUBSYS_MAX_NUM];
};

/**
 * firmware_packet - firmware packet information
 * @packet_size: firmware packet size, max 4Kbytes.
 * @flash_addr: device flash address
 * @packet_checksum: checksum of the firmware in this packet
 * @data: pointer to firmware data.
 */
struct firmware_packet {
	u32 packet_size;
	u32 flash_addr;
	u32 packet_checksum;
	const u8 *data;
};

#pragma pack()

/**
 * firmware_data - firmware data structure
 * @fw_info: firmware information
 * @firmware: firmware data structure
 */
struct firmware_data {
	struct firmware_info fw_info;
	const struct firmware *firmware;
};

enum update_status {
	UPSTA_NOTWORK = 0,
	UPSTA_PREPARING,
	UPSTA_UPDATING,
	UPSTA_ABORT,
	UPSTA_SUCCESS,
	UPSTA_FAILED
};

/**
 * fw_update_ctrl - structure used to control the
 *  firmware update process
 * @status: update status
 * @progress: indicate the progress of update
 * @allow_reset: control the reset callback
 * @allow_irq: control the irq callback
 * @allow_suspend: control the suspend callback
 * @allow_resume: allow resume callback
 * @fw_data: firmware data
 * @ts_dev: touch device
 * @fw_name: firmware name
 * @attr_fwimage: sysfs bin attrs, for storing fw image
 * @fw_from_sysfs: whether the firmware image is loadind
 *		from sysfs
 */
struct fw_update_ctrl {
	enum update_status status;
	unsigned int progress;
	bool force_update;

	bool allow_reset;
	bool allow_irq;
	bool allow_suspend;
	bool allow_resume;

	struct firmware_data fw_data;
	struct goodix_ts_device *ts_dev;

	char fw_name[FW_NAME_MAX];
	struct bin_attribute attr_fwimage;
	bool fw_from_sysfs;
};

static struct goodix_ext_module goodix_fwu_module;
/**
 * goodix_parse_firmware - parse firmware header information
 *	and subsystem information from firmware data buffer
 *
 * @fw_data: firmware struct, contains firmware header info
 *	and firmware data.
 * return: 0 - OK, < 0 - error
 */
static int goodix_parse_firmware(struct fw_update_ctrl *fwu_ctrl)
{
	const struct firmware *firmware;
	struct firmware_info *fw_info;
	struct firmware_data *fw_data = &fwu_ctrl->fw_data;
	const struct device *dev = fwu_ctrl->ts_dev->dev;
	unsigned int i, fw_offset, info_offset;
	u16 checksum;
	int r = 0;

	if (!fw_data || !fw_data->firmware) {
		dev_err(dev, "Invalid firmware data\n");
		return -EINVAL;
	}
	fw_info = &fw_data->fw_info;

	/* copy firmware head info */
	firmware = fw_data->firmware;
	if (firmware->size < FW_SUBSYS_INFO_OFFSET) {
		dev_err(dev, "Invalid firmware size:%zu\n", firmware->size);
		r = -EINVAL;
		goto err_size;
	}
	memcpy(fw_info, firmware->data, FW_SUBSYS_INFO_OFFSET);

	/* check firmware size */
	fw_info->size = be32_to_cpu(fw_info->size);
	if (firmware->size != fw_info->size + 6) {
		dev_err(dev, "Bad firmware, size not match\n");
		r = -EINVAL;
		goto err_size;
	}

	/* calculate checksum, note: sum of bytes, but check by u16 checksum */
	for (i = 6, checksum = 0; i < firmware->size; i++)
		checksum += firmware->data[i];

	/* byte order change, and check */
	fw_info->checksum = be16_to_cpu(fw_info->checksum);
	if (checksum != fw_info->checksum) {
		dev_err(dev, "Bad firmware, cheksum error\n");
		r = -EINVAL;
		goto err_size;
	}

	if (fw_info->subsys_num > FW_SUBSYS_MAX_NUM) {
		dev_err(dev, "Bad firmware, invalid subsys num\n");
		r = -EINVAL;
		goto err_size;
	}

	/* parse subsystem info */
	fw_offset = FW_HEADER_SIZE;
	for (i = 0; i < fw_info->subsys_num; i++) {
		info_offset = FW_SUBSYS_INFO_OFFSET +
					i * FW_SUBSYS_INFO_SIZE;

		fw_info->subsys[i].type = firmware->data[info_offset];
		fw_info->subsys[i].size =
			be32_to_cpup((__be32 *)&firmware->data[info_offset + 1]);
		fw_info->subsys[i].flash_addr =
			be16_to_cpup((__be16 *)&firmware->data[info_offset + 5]);
		fw_info->subsys[i].flash_addr <<= 8; /* important! */

		if (fw_offset > firmware->size) {
			dev_err(dev, "Sybsys offset exceed Firmware size\n");
			goto err_size;
		}

		fw_info->subsys[i].data = firmware->data + fw_offset;
		fw_offset += fw_info->subsys[i].size;
	}

	dev_info(dev, "Firmware package protocol: V%u\n", fw_info->protocol_ver);
	dev_info(dev, "Firmware PID:GT%s\n", fw_info->fw_pid);
	dev_info(dev, "Firmware VID:%02X%02X%02X\n", fw_info->fw_vid[0],
		 fw_info->fw_vid[1], fw_info->fw_vid[2]);
	dev_info(dev, "Firmware chip type:%02X\n", fw_info->chip_type);
	dev_info(dev, "Firmware size:%u\n", fw_info->size);
	dev_info(dev, "Firmware subsystem num:%u\n", fw_info->subsys_num);

	for (i = 0; i < fw_info->subsys_num; i++) {
		dev_dbg(dev, "Index:%d\n", i);
		dev_dbg(dev, "Subsystem type:%02X\n", fw_info->subsys[i].type);
		dev_dbg(dev, "Subsystem size:%u\n", fw_info->subsys[i].size);
		dev_dbg(dev, "Subsystem flash_addr:%08X\n",
			fw_info->subsys[i].flash_addr);
		dev_dbg(dev, "Subsystem Ptr:%p\n", fw_info->subsys[i].data);
	}

err_size:
	return r;
}

/**
 * goodix_check_update - compare the version of firmware running in
 *  touch device with the version getting from the firmware file.
 * @fw_info: firmware information to be compared
 * return: 0 firmware in the touch device needs to be updated
 *			< 0 no need to update firmware
 */
static int goodix_check_update(struct goodix_ts_device *ts_dev,
			       const struct firmware_info *fw_info)
{
	struct goodix_ts_version fw_ver = {0};
	const struct device *dev = ts_dev->dev;
	u16 fwimg_vid;
	u8 fwimg_cid;
	int r = 0;

	/* read version from chip, if we got invalid firmware version, maybe
	 * firmware in flash is incorrect, so we need to update firmware
	 */
	r = ts_dev->hw_ops->read_version(ts_dev, &fw_ver);
	if (r == -EBUS)
		return r;

	if (fw_ver.valid) {
		if (memcmp(fw_ver.pid, fw_info->fw_pid, 4)) {
			dev_err(dev, "Product ID is not match\n");
			return -EPERM;
		}

		fwimg_cid = fw_info->fw_vid[0];
		fwimg_vid = fw_info->fw_vid[1] << 8 | fw_info->fw_vid[2];
		if (fw_ver.vid == fwimg_vid && fw_ver.cid == fwimg_cid) {
			dev_err(dev, "FW version is equal to the IC's\n");
			return -EPERM;
		} else if (fw_ver.vid > fwimg_vid) {
			dev_info(dev, "Warning: fw version is lower the IC's\n");
		}
	} /* else invalid firmware, update firmware */

	dev_info(dev, "Firmware needs to be updated\n");
	return 0;
}

/**
 * goodix_reg_write_confirm - write register and confirm the value
 *  in the register.
 * @ts_dev: pointer to touch device
 * @addr: register address
 * @data: pointer to data buffer
 * @len: data length
 * return: 0 write success and confirm ok
 *		   < 0 failed
 */
static int goodix_reg_write_confirm(struct goodix_ts_device *ts_dev,
				    unsigned int addr,
				    unsigned char *data,
				    unsigned int len)
{
	u8 *cfm, cfm_buf[32];
	int r, i;

	if (len > sizeof(cfm_buf)) {
		cfm = kzalloc(len, GFP_KERNEL);
		if (!cfm)
			return -ENOMEM;
	} else {
		cfm = &cfm_buf[0];
	}

	for (i = 0; i < GOODIX_BUS_RETRY_TIMES; i++) {
		r = ts_dev->hw_ops->write(ts_dev, addr, data, len);
		if (r < 0)
			goto exit;

		r = ts_dev->hw_ops->read(ts_dev, addr, cfm, len);
		if (r < 0)
			goto exit;

		if (memcmp(data, cfm, len)) {
			r = -EMEMCMP;
			continue;
		} else {
			r = 0;
			break;
		}
	}

exit:
	if (cfm != &cfm_buf[0])
		kfree(cfm);
	return r;
}

static inline int goodix_reg_write(struct goodix_ts_device *ts_dev,
				   unsigned int addr,
				   unsigned char *data,
				   unsigned int len)
{
	return ts_dev->hw_ops->write(ts_dev, addr, data, len);
}

static inline int goodix_reg_read(struct goodix_ts_device *ts_dev,
				  unsigned int addr,
				  unsigned char *data,
				  unsigned int len)
{
	return ts_dev->hw_ops->read(ts_dev, addr, data, len);
}

/**
 * goodix_cpu_ctrl - Let cpu stay in pending state or running state
 * @ts_dev: pointer to touch device
 * @flag: control flag, which can be:
 *		CPU_CTRL_PENDING - Pending cpu
 *	Other type of control to cpu is not support.
 * return: 0 OK, < 0 Failed, -EAGAIN try again
 */
static int goodix_cpu_ctrl(struct goodix_ts_device *ts_dev, int flag)
{
	const struct device *dev = ts_dev->dev;
	u8 ctrl;
	int r;

	if (flag == CPU_CTRL_PENDING) {
		dev_info(dev, "Pending CPU\n");
		ctrl = 0x04;
	} else if (flag == CPU_CTRL_RUNNING) {
		dev_info(dev, "Running CPU\n");
		ctrl = 0x00;
	} else {
		dev_err(dev, "Invalid cpu ctrl flag\n");
		return -EPERM;
	}

	/* Pending Cpu */
	r = goodix_reg_write_confirm(ts_dev, HW_REG_CPU_EN, &ctrl, 1);
	if (unlikely(r < 0)) {
		dev_err(dev, "CPU ctrl failed:%d\n", r);
		r = -EAGAIN; /* hw reset and try again */
	}

	return r;
}

/**
 * goodix_isp_wait_stat - waitting ISP state
 * @ts_dev: pointer to touch device
 * @state: state to wait
 * return: 0 - ok, < 0 error, -ETIMEOUT timeout
 */
static int goodix_isp_wait_stat(struct goodix_ts_device *ts_dev, u16 state)
{
	const struct device *dev = ts_dev->dev;
	static u8 last_state;
	u8  isp_state;
	int i, r, err_cnt = 0;

	for (i = 0; i < 200; i++) {
		/* read isp state */
		r = goodix_reg_read(ts_dev, HW_REG_ISP_STAT,
				    &isp_state, 1);
		if (r < 0) {
			dev_err(dev, "Failed to read ISP state\n");
			if (++err_cnt > GOODIX_BUS_RETRY_TIMES)
				return r;
			continue;
		}
		err_cnt = 0;

		if (isp_state != last_state) {
			switch (isp_state) {
			case ISP_STAT_IDLE:
				dev_info(dev, "ISP state: Idle\n");
				break;
			case ISP_STAT_WRITING:
				dev_info(dev, "ISP state: Writing...\n");
				break;
			case ISP_STAT_READY:
				dev_info(dev, "ISP state: Ready to write\n");
				break;
			default:
				dev_err(dev, "ISP state: Unknown\n");
				break;
			}
		}

		last_state = isp_state;
		r = -ETIMEOUT;
		if (isp_state == state) {
			r = 0;
			break;
		}

		usleep_range(5000, 5010);
	}

	return r;
}

/**
 * goodix_isp_flash_done - check whether flash is successful
 * @ts_dev: pointer to touch device
 * return: 0 - ok, < 0 error
 */
static int goodix_isp_flash_done(struct goodix_ts_device *ts_dev)
{
	u8  isp_result;
	int r, i;

	for (i = 0; i < 2; i++) {
		r = goodix_reg_read(ts_dev, HW_REG_ISP_RESULT,
				    &isp_result, 1);
		if (r < 0) {
			/* bus error */
			break;
		} else if (isp_result == ISP_FLASH_SUCCESS) {
			dev_info(ts_dev->dev, "ISP result: OK!\n");
			r = 0;
			break;
		} else if (isp_result == ISP_FLASH_ERROR) {
			dev_err(ts_dev->dev, "ISP result: ERROR!\n");
			r = -EAGAIN;
		}
	}
	return r;
}

/**
 * goodix_isp_command - communication with ISP.
 * @cmd: ISP command.
 * return: 0 ok, <0 error
 */
static int goodix_isp_command(struct goodix_ts_device *ts_dev, u8 cmd)
{
	switch (cmd) {
	case ISP_CMD_PREPARE:
		break;
	case ISP_CMD_FLASH:
		break;
	default:
		dev_err(ts_dev->dev, "Invalid ISP cmd\n");
		return -EINVAL;
	}

	return goodix_reg_write(ts_dev, HW_REG_ISP_CMD, &cmd, 1);
}

/**
 * goodix_load_isp - load ISP program to device ram
 * @ts_dev: pointer to touch device
 * @fw_data: firmware data
 * return 0 ok, <0 error
 */
static inline int goodix_load_isp(struct goodix_ts_device *ts_dev,
				  struct firmware_data *fw_data)
{
	struct fw_subsys_info *fw_isp;
	int r;

	fw_isp = &fw_data->fw_info.subsys[0];

	dev_info(ts_dev->dev, "Loading ISP program\n");
	r = goodix_reg_write_confirm(ts_dev, HW_REG_ISP_ADDR,
				     (u8 *)fw_isp->data, fw_isp->size);
	if (r < 0)
		dev_err(ts_dev->dev, "Loading ISP error\n");

	return r;
}

/**
 * goodix_enter_update - update prepare, loading ISP program
 *  and make sure the ISP is running.
 * @fwu_ctrl: pointer to fimrware control structure
 * return: 0 ok, <0 error
 */
static int goodix_update_prepare(struct fw_update_ctrl *fwu_ctrl)
{
	struct goodix_ts_device *ts_dev = fwu_ctrl->ts_dev;
	const struct device *dev = fwu_ctrl->ts_dev->dev;
	u8 boot_val0[4] = {0xb8, 0x3f, 0x35, 0x56};
	u8 boot_val1[4] = {0xb9, 0x3e, 0xb5, 0x54};
	u8 reg_val[4] = {0x00};
	int r;

	fwu_ctrl->allow_reset = true;
	ts_dev->hw_ops->reset(ts_dev);
	fwu_ctrl->allow_reset = false;

	/* enable ILM access */
	reg_val[0] = 0x06;
	r = goodix_reg_write_confirm(ts_dev, HW_REG_ILM_ACCESS,
				     reg_val, 1);
	if (r < 0) {
		dev_err(dev, "Failed to enable ILM access\n");
		return r;
	}

	/* Pending CPU */
	r = goodix_cpu_ctrl(ts_dev, CPU_CTRL_PENDING);
	if (r < 0)
		return r;

	/* disable watchdog timer */
	reg_val[0] = 0x00;
	r = goodix_reg_write_confirm(ts_dev, HW_REG_WDT,
				     reg_val, 1);
	if (r < 0) {
		dev_err(dev, "Failed to disable watchdog\n");
		return r;
	}

	/* select bank 2 */
	reg_val[0] = 0x02;
	r = goodix_reg_write_confirm(ts_dev, HW_REG_BANK_SELECT,
				     reg_val, 1);
	if (r < 0) {
		dev_err(dev, "Failed to select bank2\n");
		return r;
	}

	/* load ISP code */
	r = goodix_load_isp(ts_dev, &fwu_ctrl->fw_data);
	if (r < 0)
		return r;

	/* Clear ISP state */
	reg_val[0] = 0x00;
	reg_val[1] = 0x00;
	r = goodix_reg_write_confirm(ts_dev, HW_REG_ISP_STAT,
				     reg_val, 2);
	if (r < 0) {
		dev_err(dev, "Failed to clear ISP state\n");
		return r;
	}

	/* set boot flag */
	reg_val[0] = 0;
	r = goodix_reg_write_confirm(ts_dev, HW_REG_BOOT_FLAG,
				     reg_val, 1);
	if (r < 0) {
		dev_err(dev, "Failed to set boot flag\n");
		return r;
	}

	/* set boot from sRam */
	r = goodix_reg_write_confirm(ts_dev, HW_REG_BOOT_CTRL0,
				     boot_val0, sizeof(boot_val0));
	if (r < 0) {
		dev_err(dev, "Failed to set boot flag\n");
		return r;
	}

	/* set boot from sRam */
	r = goodix_reg_write_confirm(ts_dev, HW_REG_BOOT_CTRL1,
				     boot_val1, sizeof(boot_val1));
	if (r < 0) {
		dev_err(dev, "Failed to set boot flag\n");
		return r;
	}

	/* disbale ILM access */
	reg_val[0] = 0x00;
	r = goodix_reg_write_confirm(ts_dev, HW_REG_ILM_ACCESS,
				     reg_val, 1);
	if (r < 0) {
		dev_err(dev, "Failed to disable ILM access\n");
		return r;
	}

	/* Release CPU */
	r = goodix_cpu_ctrl(ts_dev, CPU_CTRL_RUNNING);
	if (r < 0)
		return r;

	/* wait isp idel */
	r = goodix_isp_wait_stat(ts_dev, ISP_STAT_IDLE);
	if (r < 0) {
		dev_err(dev, "Wait ISP IDLE timeout\n");
		return r;
	}

	return r;
}

/**
 * goodix_write_fwdata - write firmware data to ISP buffer
 * @ts_dev: pointer to touch device
 * @fw_data: firmware data
 * @size: size of data, size can not exceed ISP_MAX_BUFFERSIZE
 *  + checksum size{2},
 * return: 0 ok, <0 error
 */
static int goodix_write_fwdata(struct goodix_ts_device *ts_dev,
			       const u8 *fw_data, u32 size)
{
	if (!fw_data || size > ISP_MAX_BUFFERSIZE)
		return -EINVAL;

	return  goodix_reg_write(ts_dev, HW_REG_ISP_BUFFER,
				(u8 *)fw_data, size);
}

/**
 * goodix_format_fw_packet - formate one flash packet
 * @pkt: target firmware packet
 * @flash_addr: flash address
 * @size: packet size
 * @data: packet data
 */
static int goodix_format_fw_packet(struct firmware_packet *pkt,
				   u32 flash_addr, u32 size, const u8 *data)
{
	if (!pkt || !data || size % 4)
		return -EINVAL;

	/*
	 * checksum rule:sum of data in one format is equal to zero
	 * data format: byte/le16/be16/le32/be32/le64/be64
	 */
	pkt->flash_addr = cpu_to_le32(flash_addr);
	pkt->packet_size = cpu_to_le32(size);
	pkt->packet_checksum = checksum_le32((u8 *)data, size);
	pkt->data = data;
	return 0;
}

/**
 * goodix_send_fw_packet - send one firmware packet to ISP
 * @ts_dev: target touch device
 * @pkt: firmware packet
 * return：0 ok, <0 error
 */
static int goodix_send_fw_packet(struct goodix_ts_device *ts_dev,
				 struct firmware_packet *pkt)
{
	u8 pkt_info[12];
	int r;

	if (!pkt)
		return -EINVAL;

	/* 1: wait ISP idle */
	r = goodix_isp_wait_stat(ts_dev, ISP_STAT_IDLE);
	if (r < 0)
		return r;

	/* 2: write packet information */
	memcpy(pkt_info, pkt, sizeof(pkt_info));
	r = goodix_reg_write(ts_dev, HW_REG_ISP_PKT_INFO,
			     pkt_info, sizeof(pkt_info));
	if (r < 0) {
		dev_err(ts_dev->dev, "Failed to write packet info\n");
		return r;
	}

	/* 3: Make ISP ready to flash */
	r = goodix_isp_command(ts_dev, ISP_CMD_PREPARE);
	if (r  < 0) {
		dev_err(ts_dev->dev, "Failed to make ISP ready\n");
		return r;
	}

	/* 4: write packet data(firmware block) to ISP buffer */
	r = goodix_write_fwdata(ts_dev, pkt->data, pkt->packet_size);
	if (r < 0) {
		dev_err(ts_dev->dev, "Failed to write firmware packet\n");
		return r;
	}

	/* 5: wait ISP ready */
	r = goodix_isp_wait_stat(ts_dev, ISP_STAT_READY);
	if (r < 0) {
		dev_err(ts_dev->dev, "Failed to wait ISP ready\n");
		return r;
	}

	/* 6: start writing to flash */
	r = goodix_isp_command(ts_dev, ISP_CMD_FLASH);
	if (r < 0) {
		dev_err(ts_dev->dev, "Failed to start flash\n");
		return r;
	}

	/* 7: wait idle */
	r = goodix_isp_wait_stat(ts_dev, ISP_STAT_IDLE);
	if (r < 0) {
		dev_err(ts_dev->dev, "Error occurred when wait ISP idle\n");
		return r;
	}

	/* check ISP result */
	r = goodix_isp_flash_done(ts_dev);
	if (r < 0) {
		dev_err(ts_dev->dev, "Flash fw packet failed:%d\n", r);
		return r;
	}

	return 0;
}

/**
 * goodix_flash_subsystem - flash subsystem firmware,
 *  Main flow of flashing firmware.
 *	Each firmware subsystem is divided into several
 *	packets, the max size of packet is limited to
 *	@{ISP_MAX_BUFFERSIZE}
 * @ts_dev: pointer to touch device
 * @subsys: subsystem information
 * return: 0 ok, < 0 error
 */
static int goodix_flash_subsystem(struct goodix_ts_device *ts_dev,
				  struct fw_subsys_info *subsys)
{
	struct firmware_packet fw_pkt;
	u32 data_size, total_size, offset;
	int r = 0;

	/*
	 * if bus(i2c/spi) error occued, then exit, we will do
	 * hardware reset and re-prepare ISP and then retry
	 * flashing
	 */
	total_size = subsys->size;
	offset = 0;
	while (total_size > 0) {
		data_size = total_size > ISP_MAX_BUFFERSIZE ?
				ISP_MAX_BUFFERSIZE : total_size;
		dev_info(ts_dev->dev, "Flash firmware to %08x,size:%u bytes\n",
			 subsys->flash_addr + offset, data_size);

		/* format one firmware packet */
		r = goodix_format_fw_packet(&fw_pkt, subsys->flash_addr
				+ offset, data_size, &subsys->data[offset]);
		if (r < 0) {
			dev_err(ts_dev->dev, "Invalid packet params\n");
			goto exit;
		}

		/* send one firmware packet */
		r = goodix_send_fw_packet(ts_dev, &fw_pkt);
		if (r < 0) {
			dev_err(ts_dev->dev,
				"Failed to send firmware packet,err:%d\n", r);
			goto exit;
		}

		offset += data_size;
		total_size -= data_size;
	} /* end while */

exit:
	return r;
}

/**
 * goodix_flash_firmware - flash firmware
 * @ts_dev: pointer to touch device
 * @fw_data: firmware data
 * return: 0 ok, < 0 error
 */
static int goodix_flash_firmware(struct goodix_ts_device *ts_dev,
				 struct firmware_data *fw_data)
{
	struct fw_update_ctrl *fw_ctrl;
	struct firmware_info  *fw_info;
	struct fw_subsys_info *fw_x;
	int retry = GOODIX_BUS_RETRY_TIMES;
	int i, r = 0, fw_num, prog_step;

	/* start from subsystem 1, subsystem 0 is the ISP program */
	fw_ctrl = container_of(fw_data, struct fw_update_ctrl, fw_data);
	fw_info = &fw_data->fw_info;
	fw_num = fw_info->subsys_num;

	/* we have 80% work here */
	prog_step = 80 / (fw_num - 1);

	for (i = 1; i < fw_num && retry;) {
		dev_info(ts_dev->dev,
			 "--- Start to flash subsystem[%d] ---", i);
		fw_x = &fw_info->subsys[i];
		r = goodix_flash_subsystem(ts_dev, fw_x);
		if (r == 0) {
			dev_info(ts_dev->dev,
				 "--- End flash subsystem[%d]: OK ---", i);
			fw_ctrl->progress += prog_step;
			i++;
		} else if (r == -EAGAIN) {
			retry--;
			dev_err(ts_dev->dev,
				"--- End flash subsystem%d: Fail, errno:%d, retry:%d ---",
				i, r, GOODIX_BUS_RETRY_TIMES - retry);
		} else if (r < 0) { /* bus error */
			dev_err(ts_dev->dev,
				"--- End flash subsystem%d: Fatal error:%d exit ---",
				i, r);
			goto exit_flash;
		}
	}

exit_flash:
	return r;
}

/**
 * goodix_update_finish - update finished, free resource
 *  and reset flags---
 * @fwu_ctrl: pointer to fw_update_ctrl structrue
 * return: 0 ok, < 0 error
 */
static int goodix_update_finish(struct fw_update_ctrl *fwu_ctrl)
{
	struct goodix_ts_version ver;
	int r = 0;

	fwu_ctrl->ts_dev->hw_ops->reset(fwu_ctrl->ts_dev);
	r = fwu_ctrl->ts_dev->hw_ops->read_version(fwu_ctrl->ts_dev, &ver);
	return r;
}

/**
 * goodix_fw_update_proc - firmware update process, the entry of
 *  firmware update flow
 * @fwu_ctrl: firmware control
 * return: 0 ok, < 0 error
 */
int goodix_fw_update_proc(struct fw_update_ctrl *fwu_ctrl)
{
#define FW_UPDATE_RETRY	2
	const struct device *dev = fwu_ctrl->ts_dev->dev;
	int retry0 = FW_UPDATE_RETRY, retry1 = FW_UPDATE_RETRY;
	int r = 0;

	if (fwu_ctrl->status == UPSTA_PREPARING ||
	    fwu_ctrl->status == UPSTA_UPDATING) {
		dev_err(dev, "Firmware update already in progress\n");
		return -EBUSY;
	}
	fwu_ctrl->progress = 0;
	fwu_ctrl->status = UPSTA_PREPARING;
	r = goodix_parse_firmware(fwu_ctrl);
	if (r < 0) {
		fwu_ctrl->status = UPSTA_ABORT;
		goto err_parse_fw;
	}
	fwu_ctrl->progress = 10;
	if (fwu_ctrl->force_update == false) {
		r = goodix_check_update(fwu_ctrl->ts_dev,
					&fwu_ctrl->fw_data.fw_info);
		if (r < 0) {
			fwu_ctrl->status = UPSTA_ABORT;
			goto err_check_update;
		}
	}
start_update:
	fwu_ctrl->progress = 20;
	fwu_ctrl->status = UPSTA_UPDATING; /* show upgrading status */
	r = goodix_update_prepare(fwu_ctrl);
	if ((r == -EBUS || r == -EAGAIN) && --retry0 > 0) {
		dev_err(dev, "Bus error, retry prepare ISP:%d\n",
			FW_UPDATE_RETRY - retry0);
		goto start_update;
	} else if (r < 0) {
		dev_err(dev, "Failed to prepare ISP, exit update:%d\n", r);
		fwu_ctrl->status = UPSTA_FAILED;
		goto err_fw_prepare;
	}
	/* progress: 20%~100% */
	r = goodix_flash_firmware(fwu_ctrl->ts_dev, &fwu_ctrl->fw_data);
	if ((r == -EBUS || r == -ETIMEOUT) && --retry1 > 0) {
		/* we will retry[twice] if returns bus error[i2c/spi]
		 * we will do hardware reset and re-prepare ISP and then retry
		 * flashing
		 */
		dev_err(dev, "Bus error, retry firmware update:%d\n",
			FW_UPDATE_RETRY - retry1);
		goto start_update;
	} else if (r < 0) {
		dev_err(dev, "Fatal error, exit update:%d\n", r);
		fwu_ctrl->status = UPSTA_FAILED;
		goto err_fw_flash;
	}
	fwu_ctrl->status = UPSTA_SUCCESS;
err_fw_flash:
err_fw_prepare:
	goodix_update_finish(fwu_ctrl);
err_check_update:
err_parse_fw:
	if (fwu_ctrl->status == UPSTA_SUCCESS)
		dev_info(dev, "Firmware update successfully\n");
	else if (fwu_ctrl->status == UPSTA_FAILED)
		 dev_err(dev, "Firmware update failed\n");
	fwu_ctrl->progress = 100; /* 100% */
	return r;
}

/* COMMON PART - END */

/**
 * goodix_request_firmware - request firmware data from user space
 *
 * @fw_data: firmware struct, contains firmware header info
 *	and firmware data pointer.
 * return: 0 - OK, < 0 - error
 */
static int goodix_request_firmware(struct firmware_data *fw_data,
				   const char *name)
{
	struct fw_update_ctrl *fw_ctrl =
		container_of(fw_data, struct fw_update_ctrl, fw_data);
	struct device *dev = fw_ctrl->ts_dev->dev;
	int r;

	r = request_firmware(&fw_data->firmware, name, dev);
	if (r < 0)
		dev_err(dev,
			"Firmware image [%s] not available,errno:%d\n",
			name, r);
	else
		dev_info(dev, "Firmware image [%s] is ready\n", name);
	return r;
}

/**
 * relase firmware resources
 *
 */
static inline void goodix_release_firmware(struct firmware_data *fw_data)
{
	if (fw_data->firmware) {
		release_firmware(fw_data->firmware);
		fw_data->firmware = NULL;
	}
}

static int goodix_fw_update_thread(void *data)
{
	struct fw_update_ctrl *fwu_ctrl = data;
	static DEFINE_MUTEX(fwu_lock);
	int r = -EINVAL;

	if (!fwu_ctrl)
		return r;

	if (goodix_register_ext_module(&goodix_fwu_module))
		return -EIO;

	mutex_lock(&fwu_lock);
	/* judge where to get firmware data */
	if (!fwu_ctrl->fw_from_sysfs) {
		r = goodix_request_firmware(&fwu_ctrl->fw_data,
					    fwu_ctrl->fw_name);
		if (r < 0) {
			fwu_ctrl->status = UPSTA_ABORT;
			fwu_ctrl->progress = 100;
			goto out;
		}
	} else {
		if (!fwu_ctrl->fw_data.firmware) {
			fwu_ctrl->status = UPSTA_ABORT;
			fwu_ctrl->progress = 100;
			r = -EINVAL;
			goto out;
		}
	}

	/* DONT allow reset/irq/suspend/resume during update */
	fwu_ctrl->allow_irq = false;
	fwu_ctrl->allow_suspend = false;
	fwu_ctrl->allow_resume = false;
	goodix_ts_blocking_notify(NOTIFY_FWUPDATE_START, NULL);

	/* ready to update */
	r = goodix_fw_update_proc(fwu_ctrl);

	goodix_ts_blocking_notify(NOTIFY_FWUPDATE_END, NULL);
	fwu_ctrl->allow_reset = true;
	fwu_ctrl->allow_irq = true;
	fwu_ctrl->allow_suspend = true;
	fwu_ctrl->allow_resume = true;

	/* clean */
	if (!fwu_ctrl->fw_from_sysfs) {
		goodix_release_firmware(&fwu_ctrl->fw_data);
	} else {
		fwu_ctrl->fw_from_sysfs = false;
		vfree(fwu_ctrl->fw_data.firmware);
		fwu_ctrl->fw_data.firmware = NULL;
	}

out:
	goodix_unregister_ext_module(&goodix_fwu_module);
	mutex_unlock(&fwu_lock);
	return r;
}

/* sysfs attributes */
static ssize_t goodix_sysfs_update_fw_store(
		struct goodix_ext_module *module,
		const char *buf, size_t count)
{
	int ret;

	ret = goodix_fw_update_thread(module->priv_data);
	if (ret)
		count = ret;

	return count;
}

static ssize_t goodix_sysfs_update_progress_show(
		struct goodix_ext_module *module,
		char *buf)
{
	struct fw_update_ctrl *fw_ctrl = module->priv_data;

	return scnprintf(buf, PAGE_SIZE, "%d\n", fw_ctrl->progress);
}

static ssize_t goodix_sysfs_update_result_show(
		struct goodix_ext_module *module,
		char *buf)
{
	char *result = NULL;
	struct fw_update_ctrl *fw_ctrl = module->priv_data;

	switch (fw_ctrl->status) {
	case UPSTA_NOTWORK:
		result = "notwork";
		break;
	case UPSTA_PREPARING:
		result = "preparing";
		break;
	case UPSTA_UPDATING:
		result = "upgrading";
		break;
	case UPSTA_ABORT:
		result = "abort";
		break;
	case UPSTA_SUCCESS:
		result = "success";
		break;
	case UPSTA_FAILED:
		result = "failed";
		break;
	default:
		break;
	}

	return scnprintf(buf, PAGE_SIZE, "%s\n", result);
}

static ssize_t goodix_sysfs_update_fwversion_show(
		struct goodix_ext_module *module,
		char *buf)
{
	struct goodix_ts_version fw_ver;
	struct fw_update_ctrl *fw_ctrl = module->priv_data;
	int r = 0;
	char str[5];

	/* read version from chip */
	r = fw_ctrl->ts_dev->hw_ops->read_version(fw_ctrl->ts_dev,
			&fw_ver);
	if (!r) {
		memcpy(str, fw_ver.pid, 4);
		str[4] = '\0';
		return scnprintf(buf, PAGE_SIZE,
				 "PID:%s VID:%04x SENSOR_ID:%d\n",
				 str, fw_ver.vid, fw_ver.sensor_id);
	}
	return 0;
}

static ssize_t goodix_sysfs_fwsize_show(struct goodix_ext_module *module,
					char *buf)
{
	struct fw_update_ctrl *fw_ctrl = module->priv_data;
	int r = -EINVAL;

	if (fw_ctrl && fw_ctrl->fw_data.firmware)
		r = snprintf(buf, PAGE_SIZE, "%zu\n",
			     fw_ctrl->fw_data.firmware->size);
	return r;
}

static ssize_t goodix_sysfs_fwsize_store(struct goodix_ext_module *module,
					 const char *buf, size_t count)
{
	struct fw_update_ctrl *fw_ctrl = module->priv_data;
	struct firmware *fw;
	u8 **data;
	size_t size = 0;

	if (!fw_ctrl)
		return -EINVAL;

	if (sscanf(buf, "%zu", &size) < 0 || !size) {
		dev_err(fw_ctrl->ts_dev->dev, "Failed to get fwsize");
		return -EFAULT;
	}

	fw = vmalloc(sizeof(*fw) + size);
	if (!fw)
		return -ENOMEM;

	memset(fw, 0x00, sizeof(*fw) + size);
	data = (u8 **)&fw->data;
	*data = (u8 *)fw + sizeof(struct firmware);
	fw->size = size;
	fw_ctrl->fw_data.firmware = fw;
	fw_ctrl->fw_from_sysfs = true;

	return count;
}

static ssize_t goodix_sysfs_fwimage_store(struct file *file,
					  struct kobject *kobj,
					  struct bin_attribute *attr,
					  char *buf,
					  loff_t pos,
					  size_t count)
{
	struct fw_update_ctrl *fw_ctrl;
	struct firmware_data *fw_data;

	fw_ctrl = container_of(attr, struct fw_update_ctrl,
			       attr_fwimage);
	fw_data = &fw_ctrl->fw_data;

	if (!fw_data->firmware) {
		dev_err(fw_ctrl->ts_dev->dev, "Need set fw image size first");
		return -ENOMEM;
	}

	if (fw_data->firmware->size == 0) {
		dev_err(fw_ctrl->ts_dev->dev, "Invalid firmware size");
		return -EINVAL;
	}

	if (pos + count > fw_data->firmware->size)
		return -EFAULT;

	memcpy((u8 *)&fw_data->firmware->data[pos], buf, count);
	fw_ctrl->force_update = true;

	return count;
}

static ssize_t goodix_sysfs_force_update_store(
		struct goodix_ext_module *module,
		const char *buf, size_t count)
{
	struct fw_update_ctrl *fw_ctrl = module->priv_data;
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	if (val)
		fw_ctrl->force_update = true;
	else
		fw_ctrl->force_update = false;

	return count;
}

static ssize_t goodix_sysfs_update_hwversion_show(
		struct goodix_ext_module *module,
		char *buf)
{
	struct goodix_ts_version fw_ver;
	struct fw_update_ctrl *fw_ctrl = module->priv_data;
	int r = 0;
	char str[5];

	/* read version from chip */
	r = fw_ctrl->ts_dev->hw_ops->read_version(fw_ctrl->ts_dev,
			&fw_ver);
	if (!r) {
		memcpy(str, fw_ver.pid, 4);
		str[4] = '\0';
		return scnprintf(buf, PAGE_SIZE, "%s\n", str);
	}
	return 0;
}

static ssize_t goodix_sysfs_update_fw_version_show(
		struct goodix_ext_module *module,
		char *buf)
{
	struct goodix_ts_version fw_ver;
	struct fw_update_ctrl *fw_ctrl = module->priv_data;
	int r = 0;

	/* read version from chip */
	r = fw_ctrl->ts_dev->hw_ops->read_version(fw_ctrl->ts_dev, &fw_ver);
	if (!r) {
		/*
		 * firmversion major+minor store
		 * formate is 2byte compress BCD
		 */
		return scnprintf(buf, PAGE_SIZE, "%2x.%2x\n",
				 fw_ver.vid >> 8, fw_ver.vid & 0xff);
	}
	return 0;
}

static ssize_t goodix_sysfs_fw_name_store(
		struct goodix_ext_module *module,
		const char *buf, size_t count)
{
	struct fw_update_ctrl *fwu_ctrl;

	if (!module || !module->priv_data)
		return -ENOMEM;

	fwu_ctrl = module->priv_data;
	if (count > FW_NAME_MAX) {
		dev_err(fwu_ctrl->ts_dev->dev, "Firmware name too long");
		return -EINVAL;
	}
	memset(fwu_ctrl->fw_name, 0, FW_NAME_MAX);
	memcpy(fwu_ctrl->fw_name, buf, count);

	return count;
}

static struct goodix_ext_attribute goodix_fwu_attrs[] = {
	__EXTMOD_ATTR(progress, 0444, goodix_sysfs_update_progress_show, NULL),
	__EXTMOD_ATTR(result, 0444, goodix_sysfs_update_result_show, NULL),
	__EXTMOD_ATTR(fwversion, 0444, goodix_sysfs_update_fwversion_show, NULL),
	__EXTMOD_ATTR(fwsize, 0644, goodix_sysfs_fwsize_show,
		      goodix_sysfs_fwsize_store),
	__EXTMOD_ATTR(force_update, 0200, NULL, goodix_sysfs_force_update_store),
	__EXTMOD_ATTR(update_fw, 0200, NULL, goodix_sysfs_update_fw_store),
	__EXTMOD_ATTR(fw_version, 0444, goodix_sysfs_update_fw_version_show, NULL),
	__EXTMOD_ATTR(fw_name, 0200, NULL, goodix_sysfs_fw_name_store),
	__EXTMOD_ATTR(hw_version, 0444, goodix_sysfs_update_hwversion_show, NULL),
};

static int goodix_syfs_init(struct goodix_ts_core *core_data,
			    struct goodix_ext_module *module)
{
	struct fw_update_ctrl *fw_ctrl = module->priv_data;
	const struct device *dev = &core_data->pdev->dev;
	struct kobj_type *ktype;
	int ret = 0, i;

	ktype = goodix_get_default_ktype();
	ret = kobject_init_and_add(&module->kobj, ktype,
				   &core_data->pdev->dev.kobj, "fwupdate");
	if (ret) {
		dev_err(dev, "Create fwupdate sysfs node error!\n");
		goto exit_sysfs_init;
	}

	for (i = 0; i < ARRAY_SIZE(goodix_fwu_attrs); i++) {
		if (sysfs_create_file(&module->kobj,
		    &goodix_fwu_attrs[i].attr)) {
			dev_warn(dev, "Create sysfs attr file error\n");
			kobject_put(&module->kobj);
			ret = -EINVAL;
			goto exit_sysfs_init;
		}
	}

	fw_ctrl->attr_fwimage.attr.name = "fwimage";
	fw_ctrl->attr_fwimage.attr.mode = 0200;
	fw_ctrl->attr_fwimage.size = 0;
	fw_ctrl->attr_fwimage.write = goodix_sysfs_fwimage_store;
	ret = sysfs_create_bin_file(&module->kobj,
				    &fw_ctrl->attr_fwimage);

exit_sysfs_init:
	return ret;
}

static int goodix_fw_update_init(struct goodix_ts_core *core_data,
				 struct goodix_ext_module *module)
{
	struct goodix_ts_board_data *ts_bdata = board_data(core_data);
	struct fw_update_ctrl *fwu_ctrl;
	static bool init_sysfs = true;

	if (!core_data->ts_dev)
		return -ENODEV;

	if (!module->priv_data) {
		module->priv_data = kzalloc(sizeof(*module->priv_data),
						   GFP_KERNEL);
		if (!module->priv_data)
			return -ENOMEM;
	}
	fwu_ctrl = module->priv_data;
	fwu_ctrl->ts_dev = core_data->ts_dev;
	fwu_ctrl->allow_reset = true;
	fwu_ctrl->allow_irq = true;
	fwu_ctrl->allow_suspend = true;
	fwu_ctrl->allow_resume = true;

	/* find a valid firmware image name */
	if (strlen(fwu_ctrl->fw_name) == 0) {
		if (ts_bdata && ts_bdata->fw_name)
			strlcpy(fwu_ctrl->fw_name, ts_bdata->fw_name,
				sizeof(fwu_ctrl->fw_name));
		else
			strlcpy(fwu_ctrl->fw_name, TS_DEFAULT_FIRMWARE,
				sizeof(fwu_ctrl->fw_name));
	}

	/* create sysfs interface */
	if (init_sysfs) {
		if (!goodix_syfs_init(core_data, module))
			init_sysfs = false;
	}

	return 0;
}

static int goodix_fw_update_exit(struct goodix_ts_core *core_data,
				 struct goodix_ext_module *module)
{
	return 0;
}

static int goodix_fw_before_suspend(struct goodix_ts_core *core_data,
				    struct goodix_ext_module *module)
{
	struct fw_update_ctrl *fwu_ctrl = module->priv_data;

	return fwu_ctrl->allow_suspend ? EVT_HANDLED : EVT_CANCEL_SUSPEND;
}

static int goodix_fw_before_resume(struct goodix_ts_core *core_data,
				struct goodix_ext_module *module)
{
	struct fw_update_ctrl *fwu_ctrl = module->priv_data;

	return fwu_ctrl->allow_resume ?
				EVT_HANDLED : EVT_CANCEL_RESUME;
}

static int goodix_fw_irq_event(struct goodix_ts_core *core_data,
			       struct goodix_ext_module *module)
{
	struct fw_update_ctrl *fwu_ctrl = module->priv_data;

	return fwu_ctrl->allow_irq ?
				EVT_HANDLED : EVT_CANCEL_IRQEVT;
}

static int goodix_fw_before_reset(struct goodix_ts_core *core_data,
				  struct goodix_ext_module *module)
{
	struct fw_update_ctrl *fwu_ctrl = module->priv_data;

	return fwu_ctrl->allow_reset ? EVT_HANDLED : EVT_CANCEL_RESET;
}

static const struct goodix_ext_module_funcs goodix_ext_funcs = {
	.init = goodix_fw_update_init,
	.exit = goodix_fw_update_exit,
	.before_reset = goodix_fw_before_reset,
	.after_reset = NULL,
	.before_suspend = goodix_fw_before_suspend,
	.after_suspend = NULL,
	.before_resume = goodix_fw_before_resume,
	.after_resume = NULL,
	.irq_event = goodix_fw_irq_event,
};

static struct goodix_ext_module goodix_fwu_module = {
	.name = "goodix-fwu",
	.funcs = &goodix_ext_funcs,
	.priority = EXTMOD_PRIO_FWUPDATE,
};

static int __init goodix_fwu_module_init(void)
{
	return goodix_register_ext_module(&goodix_fwu_module);
}

static void __exit goodix_fwu_module_exit(void)
{
}

module_init(goodix_fwu_module_init);
module_exit(goodix_fwu_module_exit);

MODULE_DESCRIPTION("Goodix FWU Module");
MODULE_AUTHOR("Goodix, Inc.");
MODULE_LICENSE("GPL v2");
