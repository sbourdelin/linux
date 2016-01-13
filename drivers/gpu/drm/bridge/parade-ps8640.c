/*
 * Copyright (c) 2014 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>
#include <asm/unaligned.h>
#include <drm/drm_panel.h>

#include <drmP.h>
#include <drm_crtc_helper.h>
#include <drm_crtc.h>
#include <drm_atomic_helper.h>

#define PAGE2_SPI_CFG3		0x82
#define I2C_TO_SPI_RESET	0x20
#define PAGE2_ROMADD_BYTE1	0x8e
#define PAGE2_ROMADD_BYTE2	0x8f
#define PAGE2_SWSPI_WDATA	0x90
#define PAGE2_SWSPI_RDATA	0x91
#define PAGE2_SWSPI_LEN		0x92
#define PAGE2_SWSPI_CTL		0x93
#define TRIGGER_NO_READBACK	0x05
#define TRIGGER_READBACK	0x01
#define PAGE2_SPI_STATUS	0x9e
#define PAGE2_GPIO_L		0xa6
#define PAGE2_GPIO_H		0xa7
#define PS_GPIO9		BIT(1)
#define PAGE2_IROM_CTRL		0xb0
#define IROM_ENABLE		0xc0
#define IROM_DISABLE		0x80
#define PAGE2_SW_REST		0xbc
#define PAGE2_ENCTLSPI_WR	0xda
#define PAGE2_I2C_BYPASS	0xea
#define I2C_BYPASS_EN		0xd0

#define PAGE3_SET_ADD		0xfe
#define PAGE3_SET_VAL		0xff
#define VDO_CTL_ADD		0x13
#define VDO_DIS			0x18
#define VDO_EN			0x1c

#define PAGE4_REV_L		0xf0
#define PAGE4_REV_H		0xf1
#define PAGE4_CHIP_L		0xf2
#define PAGE4_CHIP_H		0xf3

/* Firmware */
#define SPI_MAX_RETRY_CNT	8
#define PS_FW_NAME		"ps864x_fw.bin"

#define FW_CHIP_ID_OFFSET		0
#define FW_VERSION_OFFSET		2

#define bridge_to_ps8640(e)	container_of(e, struct ps8640, bridge)
#define connector_to_ps8640(e)	container_of(e, struct ps8640, connector)

struct ps8640_info {
	u8 family_id;
	u8 variant_id;
	u16 version;
};

struct ps8640 {
	struct drm_connector connector;
	struct drm_bridge bridge;
	struct i2c_client *page[8];
	struct ps8640_driver_data *driver_data;
	struct regulator *pwr_1v2_supply;
	struct regulator *pwr_3v3_supply;
	struct drm_panel *panel;
	struct gpio_desc *gpio_rst_n;
	struct gpio_desc *gpio_slp_n;
	struct gpio_desc *gpio_mode_sel_n;
	bool enabled;

	/* firmware file name */
	bool in_fw_update;
	char *fw_file;
	struct ps8640_info info;
};

static const u8 enc_ctrl_code[6] = {0xaa, 0x55, 0x50, 0x41, 0x52, 0x44};

static int ps8640_regr(struct i2c_client *client, u8 reg, u8 *data,
		       u16 data_len)
{
	int ret;

	struct i2c_msg msgs[] = {
		{
		 .addr = client->addr,
		 .flags = 0,
		 .len = 1,
		 .buf = &reg,
		 },
		{
		 .addr = client->addr,
		 .flags = I2C_M_RD,
		 .len = data_len,
		 .buf = data,
		 }
	};

	ret = i2c_transfer(client->adapter, msgs, 2);

	if (ret == 2)
		return 0;
	if (ret < 0)
		return ret;
	else
		return -EIO;
}

static int ps8640_regw(struct i2c_client *client, u8 reg,  const u8 *data,
		       u16 data_len)
{
	int ret, i;
	struct i2c_msg msg;
	u8 *buf;

	buf = devm_kzalloc(&client->dev, data_len + 1, GFP_KERNEL);
	/* i2c page size is 256 bytes, so limit the data_len 256 */
	if (data_len > 256) {
		dev_err(&client->dev, "data_len must under 256: len = %d\n",
			data_len);
		return -EIO;
	}
	for (i = 0; i < data_len + 1; i++)
		buf[i] = (i == 0) ? reg : data[i - 1];

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = data_len + 1;
	msg.buf = buf;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret == 1)
		return 0;
	if (ret < 0)
		return ret;
	else
		return -EIO;
}

static int ps8640_regw_byte(struct i2c_client *client, u8 reg,  u8 data)
{
	int ret;
	struct i2c_msg msg;
	u8 buf[] = {reg, data};

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = sizeof(buf);
	msg.buf = buf;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret == 1)
		return 0;
	if (ret < 0)
		return ret;
	else
		return -EIO;
}

static int ps8640_check_valid_id(struct ps8640 *ps_bridge)
{
	struct i2c_client *client = ps_bridge->page[4];
	u8 chip_id[4];

	ps8640_regr(client, PAGE4_REV_L, chip_id, 4);

	if ((chip_id[0] == 0x00) && (chip_id[1] == 0x0a) &&
	    (chip_id[2] == 0x00) && (chip_id[3] == 0x30))
		return 0;

	return -ENODEV;
}

static void ps8640_show_mcu_fw_version(struct ps8640 *ps_bridge)
{
	struct i2c_client *client = ps_bridge->page[5];
	u8 fw_ver[2];

	ps8640_regr(client, 0x4, fw_ver, 2);
	ps_bridge->info.version = (fw_ver[0] << 8) | fw_ver[1];

	DRM_INFO_ONCE("ps8640 rom fw version %d.%d\n", fw_ver[0], fw_ver[1]);
}

static int ps8640_bdg_enable(struct ps8640 *ps_bridge)
{
	struct i2c_client *client = ps_bridge->page[3];
	int ret;
	u8 vdo_ctrl[2] = {VDO_CTL_ADD, VDO_EN};

	ret = ps8640_check_valid_id(ps_bridge);
	if (ret) {
		DRM_ERROR("ps8640 not valid: %d\n", ret);
		return ret;
	}

	ret = ps8640_regw(client, PAGE3_SET_ADD, vdo_ctrl, 2);
	return ret;
}

static bool ps8640_in_bootloader(struct ps8640 *ps_bridge)
{
	return ps_bridge->in_fw_update;
}

static void ps8640_prepare(struct ps8640 *ps_bridge)
{
	struct i2c_client *client = ps_bridge->page[2];
	int err, retry_cnt = 0;
	u8 set_vdo_done;

	if (ps8640_in_bootloader(ps_bridge))
		return;

	if (ps_bridge->enabled)
		return;

	err = drm_panel_prepare(ps_bridge->panel);
	if (err < 0) {
		DRM_ERROR("failed to prepare panel: %d\n", err);
		return;
	}

	/* delay for power stable */
	usleep_range(500, 700);

	err = regulator_enable(ps_bridge->pwr_1v2_supply);
	if (err < 0) {
		DRM_ERROR("failed to enable vdd12-supply: %d\n", err);
		goto err_panel_unprepare;
	}

	err = regulator_enable(ps_bridge->pwr_3v3_supply);
	if (err < 0) {
		DRM_ERROR("failed to enable vdd33-supply: %d\n", err);
		goto err_regulator_1v2_disable;
	}

	gpiod_set_value(ps_bridge->gpio_slp_n, 1);
	gpiod_set_value(ps_bridge->gpio_rst_n, 0);
	usleep_range(500, 700);
	gpiod_set_value(ps_bridge->gpio_rst_n, 1);

	/* wait for the ps8640 embed mcu ready
	  * first wait 200ms and then check the mcu ready flag every 20ms
	*/
	msleep(200);
	do {
		err = ps8640_regr(client, PAGE2_GPIO_H, &set_vdo_done, 1);
		if (err < 0) {
			DRM_ERROR("failed read PAGE2_GPIO_H: %d\n", err);
			goto err_regulator_1v2_disable;
		}
		msleep(20);
	} while ((retry_cnt++ < 10) && ((set_vdo_done & PS_GPIO9) != PS_GPIO9));

	ps8640_show_mcu_fw_version(ps_bridge);
	ps8640_regw_byte(client, PAGE2_I2C_BYPASS, I2C_BYPASS_EN);
	ps_bridge->enabled = true;

	return;

err_regulator_1v2_disable:
	regulator_disable(ps_bridge->pwr_1v2_supply);
err_panel_unprepare:
	drm_panel_unprepare(ps_bridge->panel);
}

static void ps8640_pre_enable(struct drm_bridge *bridge)
{
	struct ps8640 *ps_bridge = bridge_to_ps8640(bridge);

	ps8640_prepare(ps_bridge);
}

static void ps8640_enable(struct drm_bridge *bridge)
{
	struct ps8640 *ps_bridge = bridge_to_ps8640(bridge);
	int err;

	err = ps8640_bdg_enable(ps_bridge);
	if (err)
		DRM_ERROR("failed to enable unmutevideo: %d\n", err);

	err = drm_panel_enable(ps_bridge->panel);
	if (err < 0)
		DRM_ERROR("failed to enable panel: %d\n", err);
}

static void ps8640_disable(struct drm_bridge *bridge)
{
	struct ps8640 *ps_bridge = bridge_to_ps8640(bridge);
	int err;

	if (ps8640_in_bootloader(ps_bridge))
		return;

	if (!ps_bridge->enabled)
		return;

	ps_bridge->enabled = false;

	err = drm_panel_disable(ps_bridge->panel);
	if (err < 0)
		DRM_ERROR("failed to disable panel: %d\n", err);

	gpiod_set_value(ps_bridge->gpio_rst_n, 0);
	gpiod_set_value(ps_bridge->gpio_slp_n, 0);
	regulator_disable(ps_bridge->pwr_3v3_supply);
	regulator_disable(ps_bridge->pwr_1v2_supply);
}

static void ps8640_post_disable(struct drm_bridge *bridge)
{
	struct ps8640 *ps_bridge = bridge_to_ps8640(bridge);
	int err;

	err = drm_panel_unprepare(ps_bridge->panel);
	if (err)
		DRM_ERROR("failed to unprepare panel: %d\n", err);
}

static int ps8640_get_modes(struct drm_connector *connector)
{
	struct ps8640 *ps_bridge = connector_to_ps8640(connector);

	ps8640_prepare(ps_bridge);

	return drm_panel_get_modes(ps_bridge->panel);
}

static struct drm_encoder *ps8640_best_encoder(struct drm_connector *connector)
{
	struct ps8640 *ps_bridge;

	ps_bridge = connector_to_ps8640(connector);
	return ps_bridge->bridge.encoder;
}

static const struct drm_connector_helper_funcs ps8640_connector_helper_funcs = {
	.get_modes = ps8640_get_modes,
	.best_encoder = ps8640_best_encoder,
};

static enum drm_connector_status ps8640_detect(struct drm_connector *connector,
					       bool force)
{
	return connector_status_connected;
}

static void ps8640_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs ps8640_connector_funcs = {
	.dpms = drm_atomic_helper_connector_dpms,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.detect = ps8640_detect,
	.destroy = ps8640_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

int ps8640_bridge_attach(struct drm_bridge *bridge)
{
	struct ps8640 *ps_bridge = bridge_to_ps8640(bridge);
	int ret;

	if (!bridge->encoder) {
		DRM_ERROR("Parent encoder object not found");
		return -ENODEV;
	}

	ret = drm_connector_init(bridge->dev, &ps_bridge->connector,
				 &ps8640_connector_funcs,
				 DRM_MODE_CONNECTOR_eDP);

	if (ret) {
		DRM_ERROR("Failed to initialize connector with drm: %d\n", ret);
		return ret;
	}

	drm_connector_helper_add(&ps_bridge->connector,
				 &ps8640_connector_helper_funcs);
	drm_connector_register(&ps_bridge->connector);

	ps_bridge->connector.dpms = DRM_MODE_DPMS_ON;
	drm_mode_connector_attach_encoder(&ps_bridge->connector,
					  bridge->encoder);

	if (ps_bridge->panel)
		drm_panel_attach(ps_bridge->panel, &ps_bridge->connector);

	return ret;
}

static bool ps8640_bridge_mode_fixup(struct drm_bridge *bridge,
				     const struct drm_display_mode *mode,
				     struct drm_display_mode *adjusted_mode)
{
	return true;
}

static const struct drm_bridge_funcs ps8640_bridge_funcs = {
	.attach = ps8640_bridge_attach,
	.mode_fixup = ps8640_bridge_mode_fixup,
	.disable = ps8640_disable,
	.post_disable = ps8640_post_disable,
	.pre_enable = ps8640_pre_enable,
	.enable = ps8640_enable,
};

static ssize_t ps8640_fw_file_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct ps8640 *ps_bridge = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%s\n", ps_bridge->fw_file);
}

static int ps8640_update_file_name(struct device *dev, char **file_name,
				   const char *buf, size_t count)
{
	char *new_file_name;

	/* Simple sanity check */
	if (count > 64) {
		dev_warn(dev, "File name too long\n");
		return -EINVAL;
	}

	new_file_name = devm_kmalloc(dev, count + 1, GFP_KERNEL);
	if (!new_file_name)
		return -ENOMEM;

	memcpy(new_file_name, buf, count + 1);

	/* Echo into the sysfs entry may append newline at the end of buf */
	if (new_file_name[count - 1] == '\n')
		count--;

	new_file_name[count] = '\0';

	if (*file_name)
		devm_kfree(dev, *file_name);

	*file_name = new_file_name;

	return 0;
}

static ssize_t ps8640_fw_file_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct ps8640 *ps_bridge = dev_get_drvdata(dev);
	int ret;

	ret = ps8640_update_file_name(dev, &ps_bridge->fw_file, buf, count);
	if (ret)
		return ret;

	return count;
}

/* Firmware Version is returned as Major.Minor.Build */
static ssize_t ps8640_fw_version_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct ps8640 *ps_bridge = dev_get_drvdata(dev);
	struct ps8640_info *info = &ps_bridge->info;

	return scnprintf(buf, PAGE_SIZE, "%u.%u\n", info->version >> 8,
			 info->version & 0xff);
}

/* Hardware Version is returned as FamilyID.VariantID */
static ssize_t ps8640_hw_version_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct ps8640 *ps_bridge = dev_get_drvdata(dev);
	struct ps8640_info *info = &ps_bridge->info;

	return scnprintf(buf, PAGE_SIZE, "ps%u.%u\n", info->family_id,
			 info->variant_id);
}

static int ps8640_spi_send_cmd(struct ps8640 *ps_bridge, u8 *cmd, u8 cmd_len)
{
	struct i2c_client *client = ps_bridge->page[2];
	u8 i;
	int ret;

	ret = ps8640_regw_byte(client, PAGE2_IROM_CTRL, IROM_ENABLE);
	if (ret)
		goto err;

	/* write command in write port */
	for (i = 0; i < cmd_len; i++) {
		ret = ps8640_regw_byte(client, PAGE2_SWSPI_WDATA, cmd[i]);
		if (ret)
			goto err;
	}
	/*	command length */
	ret = ps8640_regw_byte(client, PAGE2_SWSPI_LEN, cmd_len - 1);
	if (ret)
		goto err;
	/*	trigger read */
	ret = ps8640_regw_byte(client, PAGE2_SWSPI_CTL, TRIGGER_NO_READBACK);
	if (ret)
		goto err;

	ret = ps8640_regw_byte(client, PAGE2_IROM_CTRL, IROM_DISABLE);
	if (ret)
		goto err;

	return 0;

err:
	dev_err(&client->dev, "send command err: %d\n", ret);
	return ret;
}

static int ps8640_wait_spi_ready(struct ps8640 *ps_bridge)
{
	struct i2c_client *client = ps_bridge->page[2];
	u8 spi_rdy_st, retry_cnt = 0;

	do {
		ps8640_regr(client, PAGE2_SPI_STATUS, &spi_rdy_st, 1);
		msleep(20);
		if ((retry_cnt == SPI_MAX_RETRY_CNT) &&
		    ((spi_rdy_st & 0x0c) != 0x0c)) {
			dev_err(&client->dev, "wait spi ready timeout\n");
			return -EBUSY;
		}
	} while ((retry_cnt++ < SPI_MAX_RETRY_CNT) &&
		 (spi_rdy_st & 0x0c) == 0x0c);

	return 0;
}

static int ps8640_wait_spi_nobusy(struct ps8640 *ps_bridge)
{
	struct i2c_client *client = ps_bridge->page[2];
	u8 spi_status, retry_cnt = 0;
	int ret;

	do {
		/*  0x05 RDSR; Read-Status-Register */
		ret = ps8640_regw_byte(client, PAGE2_SWSPI_WDATA, 0x05);
		if (ret)
			goto err_send_cmd_exit;

		/*  command length */
		ret = ps8640_regw_byte(client, PAGE2_SWSPI_LEN, 0x00);
		if (ret)
			goto err_send_cmd_exit;

		/*  trigger read */
		ret = ps8640_regw_byte(client, PAGE2_SWSPI_CTL,
				       TRIGGER_READBACK);
		if (ret)
			goto err_send_cmd_exit;

		/* delay for cmd send */
		usleep_range(100, 300);
		/* wait for SPI ROM until not busy */
		ret = ps8640_regr(client, PAGE2_SWSPI_RDATA, &spi_status, 1);
		if (ret)
			goto err_send_cmd_exit;
	} while ((retry_cnt++ < SPI_MAX_RETRY_CNT) &&
		 (spi_status & 0x0c) == 0x0c);

	if ((retry_cnt > SPI_MAX_RETRY_CNT) && (spi_status & 0x0c) != 0x0c) {
		ret = -EBUSY;
		dev_err(&client->dev, "wait spi no busy timeout: %d\n", ret);
		goto err_timeout;
	}

	return 0;

err_send_cmd_exit:
	dev_err(&client->dev, "send command err: %d\n", ret);

err_timeout:
	return ret;
}

static int ps8640_wait_rom_idle(struct ps8640 *ps_bridge)
{
	struct i2c_client *client = ps_bridge->page[0];
	int ret;

	ret = ps8640_regw_byte(client, PAGE2_IROM_CTRL, IROM_ENABLE);
	if (ret)
		goto exit;

	ret = ps8640_wait_spi_ready(ps_bridge);
	if (ret)
		goto exit;

	ret = ps8640_wait_spi_nobusy(ps_bridge);
	if (ret)
		goto exit;

	ret = ps8640_regw_byte(client, PAGE2_IROM_CTRL, IROM_DISABLE);
	if (ret)
		goto exit;

exit:
	if (ret)
		dev_err(&client->dev, "wait ps8640 rom idle fail: %d\n", ret);

	return ret;
}

static int ps8640_spi_dl_mode(struct ps8640 *ps_bridge)
{
	struct i2c_client *client = ps_bridge->page[2];
	int ret;

	/* switch ps8640 mode to spi dl mode */
	gpiod_set_value(ps_bridge->gpio_mode_sel_n, 0);

	/* reset spi interface */
	ret = ps8640_regw_byte(client, PAGE2_SW_REST, 0xc0);
	if (ret)
		goto exit;

	ret = ps8640_regw_byte(client, PAGE2_SW_REST, 0x40);
	if (ret)
		goto exit;

exit:
	if (ret)
		dev_err(&client->dev, "fail reset spi interface: %d\n", ret);

	return ret;
}

static int ps8640_rom_prepare(struct ps8640 *ps_bridge)
{
	struct i2c_client *client = ps_bridge->page[2];
	struct device *dev = &client->dev;
	u8 i, cmd[2];
	int ret;

	/*	Enable-Write-Status-Register */
	cmd[0] = 0x06;
	ret = ps8640_spi_send_cmd(ps_bridge, cmd, 1);
	if (ret) {
		dev_err(dev, "failed enable-write-status-register: %d\n", ret);
		return ret;
	}

	/* disable all protection */
	cmd[0] = 0x01;
	cmd[1] = 0x00;
	ret = ps8640_spi_send_cmd(ps_bridge, cmd, 2);
	if (ret) {
		dev_err(dev, "fail disable all protection: %d\n", ret);
		return ret;
	}

	/* wait for SPI module ready */
	ret = ps8640_wait_rom_idle(ps_bridge);
	if (ret) {
		dev_err(dev, "fail wait rom idle: %d\n", ret);
		return ret;
	}

	ps8640_regw_byte(client, PAGE2_IROM_CTRL, IROM_ENABLE);
	for (i = 0; i < 6; i++)
		ps8640_regw_byte(client, PAGE2_ENCTLSPI_WR, enc_ctrl_code[i]);
	ps8640_regw_byte(client, PAGE2_IROM_CTRL, IROM_DISABLE);

	/*	Enable-Write-Status-Register */
	cmd[0] = 0x06;
	ret = ps8640_spi_send_cmd(ps_bridge, cmd, 1);
	if (ret) {
		dev_err(dev, "fail enable-write-status-register: %d\n", ret);
		return ret;
	}

	/* chip erase command */
	cmd[0] = 0xc7;
	ret = ps8640_spi_send_cmd(ps_bridge, cmd, 1);
	if (ret) {
		dev_err(dev, "fail disable all protection: %d\n", ret);
		return ret;
	}

	ret = ps8640_wait_rom_idle(ps_bridge);
	if (ret) {
		dev_err(dev, "fail wait rom idle: %d\n", ret);
		return ret;
	}

	return 0;
}

static int ps8640_validate_firmware(struct ps8640 *ps_bridge,
				    const struct firmware *fw)
{
	struct i2c_client *client = ps_bridge->page[0];
	struct ps8640_info *info = &ps_bridge->info;
	u16 fw_chip_id, fw_version_id;

	/*
	 * Get the chip_id from the firmware. Make sure that it is the
	 * right controller to do the firmware and config update.
	 */
	fw_chip_id = get_unaligned_le16(fw->data + FW_CHIP_ID_OFFSET);

	if (fw_chip_id != 0x8640) {
		dev_err(&client->dev,
			"chip id mismatch: fw 0x%x vs. chip 0x8640\n",
			fw_chip_id);
		return -ENODEV;
	}

	fw_version_id = get_unaligned_le16(fw->data + FW_VERSION_OFFSET);

	if (fw_chip_id != info->version) {
		dev_err(&client->dev,
			"fw version mismatch: fw %d.%d vs. chip %d.%d\n",
			fw_version_id >> 8, fw_version_id & 0xff,
			info->version >> 8, info->version & 0xff);
		return -ENODEV;
	}

	return 0;
}

static int ps8640_write_rom(struct ps8640 *ps_bridge, const struct firmware *fw)
{
	struct i2c_client *client = ps_bridge->page[0];
	struct device *dev = &client->dev;
	struct i2c_client *client2 = ps_bridge->page[2];
	struct i2c_client *client7 = ps_bridge->page[7];
	unsigned int pos = 0;
	u8 progress_cnt, rom_page_id[2];
	int ret;

	ps8640_regw_byte(client2, PAGE2_SPI_CFG3, I2C_TO_SPI_RESET);
	msleep(100);
	ps8640_regw_byte(client2, PAGE2_SPI_CFG3, 0x00);

	while (pos < fw->size) {
		rom_page_id[0] = (pos >> 8) & 0xFF;
		rom_page_id[1] = (pos >> 16) & 0xFF;
		ret = ps8640_regw(client2, PAGE2_ROMADD_BYTE1, rom_page_id, 2);
		if (ret)
			goto error;
		ret = ps8640_regw(client7, 0, fw->data + pos, 256);
		if (ret)
			goto error;

		pos += 256;
		if (progress_cnt != (pos * 100) / fw->size) {
			progress_cnt = (pos * 100) / fw->size;
			dev_info(dev, "fw update progress percent %d\n",
				 progress_cnt);
		}
	}
	return 0;

error:
	dev_err(dev, "failed write extenal flash, %d\n", ret);
	return ret;
}

static int ps8640_spi_normal_mode(struct ps8640 *ps_bridge)
{
	u8 cmd[2];
	struct i2c_client *client = ps_bridge->page[2];

	/*  Enable-Write-Status-Register */
	cmd[0] = 0x06;
	ps8640_spi_send_cmd(ps_bridge, cmd, 1);

	/* protect BPL/BP0/BP1 */
	cmd[0] = 0x01;
	cmd[1] = 0x8c;
	ps8640_spi_send_cmd(ps_bridge, cmd, 2);

	/* wait for SPI rom ready */
	ps8640_wait_rom_idle(ps_bridge);

     /* disable PS8640 mapping function */
	ps8640_regw_byte(client, PAGE2_ENCTLSPI_WR, 0x00);

	gpiod_set_value(ps_bridge->gpio_mode_sel_n, 1);
	return 0;
}

static int ps8640_enter_bl(struct ps8640 *ps_bridge)
{
	int ret;

	ret = ps8640_spi_dl_mode(ps_bridge);
	if (ret)
		return ret;

	ps_bridge->in_fw_update = true;
	return 0;
}

static void ps8640_exit_bl(struct ps8640 *ps_bridge, const struct firmware *fw)
{
	ps_bridge->in_fw_update = false;
	ps8640_spi_normal_mode(ps_bridge);
}

static int ps8640_load_fw(struct ps8640 *ps_bridge, const struct firmware *fw)
{
	struct i2c_client *client = ps_bridge->page[0];
	struct device *dev = &client->dev;
	int ret;
	bool ps8640_status_backup = ps_bridge->enabled;

	if (!ps8640_in_bootloader(ps_bridge)) {
		if (!ps8640_status_backup)
			ps8640_prepare(ps_bridge);

		ret = ps8640_enter_bl(ps_bridge);
		if (ret)
			goto exit;
	}

	ret = ps8640_validate_firmware(ps_bridge, fw);
	if (ret)
		goto exit;

	ret = ps8640_rom_prepare(ps_bridge);
	if (ret)
		goto exit;

	ret = ps8640_write_rom(ps_bridge, fw);

exit:
	if (ret)
		dev_err(dev, "Failed to load firmware, %d\n", ret);

	ps8640_exit_bl(ps_bridge, fw);
	if (!ps8640_status_backup)
		ps8640_disable(&ps_bridge->bridge);
	return ret;
}

static ssize_t ps8640_update_fw_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ps8640 *ps_bridge = i2c_get_clientdata(client);
	const struct firmware *fw;
	int error;

	error = request_firmware(&fw, ps_bridge->fw_file, dev);
	if (error) {
		dev_err(dev, "Unable to open firmware %s: %d\n",
			ps_bridge->fw_file, error);
		return error;
	}

	error = ps8640_load_fw(ps_bridge, fw);
	if (error)
		dev_err(dev, "The firmware update failed(%d)\n", error);
	else
		dev_info(dev, "The firmware update succeeded\n");

	release_firmware(fw);
	return error ? error : count;
}

static DEVICE_ATTR(fw_file, S_IRUGO | S_IWUSR, ps8640_fw_file_show,
		   ps8640_fw_file_store);
static DEVICE_ATTR(fw_version, S_IRUGO, ps8640_fw_version_show, NULL);
static DEVICE_ATTR(hw_version, S_IRUGO, ps8640_hw_version_show, NULL);
static DEVICE_ATTR(update_fw, S_IWUSR, NULL, ps8640_update_fw_store);

static struct attribute *ps8640_attrs[] = {
	&dev_attr_fw_file.attr,
	&dev_attr_fw_version.attr,
	&dev_attr_hw_version.attr,
	&dev_attr_update_fw.attr,
	NULL
};

static const struct attribute_group ps8640_attr_group = {
	.attrs = ps8640_attrs,
};

static void ps8640_remove_sysfs_group(void *data)
{
	struct ps8640 *ps_bridge = data;

	sysfs_remove_group(&ps_bridge->page[0]->dev.kobj, &ps8640_attr_group);
}

/* for fireware update end */

static int ps8640_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct ps8640 *ps_bridge;
	struct device_node *np = dev->of_node;
	struct device_node *port, *out_ep;
	struct device_node *panel_node = NULL;
	int i, ret;

	ps_bridge = devm_kzalloc(dev, sizeof(*ps_bridge), GFP_KERNEL);
	if (!ps_bridge) {
		ret = -ENOMEM;
		goto exit;
	}
	/* port@1 is ps8640 output port */
	port = of_graph_get_port_by_id(np, 1);
	if (port) {
		out_ep = of_get_child_by_name(port, "endpoint");
		of_node_put(port);
		if (out_ep) {
			panel_node = of_graph_get_remote_port_parent(out_ep);
			of_node_put(out_ep);
		}
	}
	if (panel_node) {
		ps_bridge->panel = of_drm_find_panel(panel_node);
		of_node_put(panel_node);
		if (!ps_bridge->panel) {
			ret = -EPROBE_DEFER;
			goto exit;
		}
	}

	ps_bridge->pwr_3v3_supply = devm_regulator_get(dev, "vdd33");
	if (IS_ERR(ps_bridge->pwr_3v3_supply)) {
		ret = PTR_ERR(ps_bridge->pwr_3v3_supply);
		dev_err(dev, "cannot get vdd33 supply: %d\n", ret);
		goto exit;
	}

	ps_bridge->pwr_1v2_supply = devm_regulator_get(dev, "vdd12");
	if (IS_ERR(ps_bridge->pwr_1v2_supply)) {
		ret = PTR_ERR(ps_bridge->pwr_1v2_supply);
		dev_err(dev, "cannot get vdd12 supply: %d\n", ret);
		goto exit;
	}

	ps_bridge->gpio_mode_sel_n = devm_gpiod_get(&client->dev, "mode-sel",
						    GPIOD_OUT_HIGH);
	if (IS_ERR(ps_bridge->gpio_mode_sel_n)) {
		ret = PTR_ERR(ps_bridge->gpio_mode_sel_n);
		dev_err(dev, "cannot get gpio_mode_sel_n %d\n", ret);
		goto exit;
	}

	ps_bridge->gpio_slp_n = devm_gpiod_get(&client->dev, "sleep",
					       GPIOD_OUT_HIGH);
	if (IS_ERR(ps_bridge->gpio_slp_n)) {
		ret = PTR_ERR(ps_bridge->gpio_slp_n);
		dev_err(dev, "cannot get gpio_slp_n: %d\n", ret);
		goto exit;
	}

	ps_bridge->gpio_rst_n = devm_gpiod_get(&client->dev, "reset",
					       GPIOD_OUT_HIGH);
	if (IS_ERR(ps_bridge->gpio_rst_n)) {
		ret = PTR_ERR(ps_bridge->gpio_rst_n);
		dev_err(dev, "cannot get gpio_rst_n: %d\n", ret);
		goto exit;
	}

	ps_bridge->bridge.funcs = &ps8640_bridge_funcs;
	ps_bridge->bridge.of_node = dev->of_node;
	ret = drm_bridge_add(&ps_bridge->bridge);
	if (ret) {
		dev_err(dev, "Failed to add bridge: %d\n", ret);
		goto exit;
	}

	ret = ps8640_update_file_name(&client->dev, &ps_bridge->fw_file,
				      PS_FW_NAME, strlen(PS_FW_NAME));
	if (ret) {
		dev_err(dev, "failed to update file name: %d\n", ret);
		goto exit;
	}

	ps_bridge->page[0] = client;

	/* ps8640 uses multiple addresses, use dummy devices for them
	  * page[0]: for DP control
	  * page[1]: for VIDEO Bridge
	  * page[2]: for control top
	  * page[3]: for DSI Link Control1
	  * page[4]: for MIPI Phy
	  * page[5]: for VPLL
	  * page[6]: for DSI Link Control2
	  * page[7]: for spi rom mapping
	*/
	for (i = 1; i < 8; i++) {
		ps_bridge->page[i] = i2c_new_dummy(client->adapter,
						   client->addr + i);
		if (!ps_bridge->page[i]) {
			dev_err(dev, "failed i2c dummy device, address%02x\n",
				client->addr + i);
			ret = -EBUSY;
			goto exit_dummy;
		}
	}
	i2c_set_clientdata(client, ps_bridge);

	ret = sysfs_create_group(&client->dev.kobj, &ps8640_attr_group);
	if (ret) {
		dev_err(dev, "failed to create sysfs entries: %d\n", ret);
		goto exit_dummy;
	}

	ret = devm_add_action(dev, ps8640_remove_sysfs_group, ps_bridge);
	if (ret) {
		ps8640_remove_sysfs_group(ps_bridge);
		dev_err(dev, "failed to add sysfs cleanup action: %d\n", ret);
		goto exit_dummy;
	}
	return 0;

exit_dummy:
	for (i = 1; i < 8; i++)
		if (ps_bridge->page[i])
			i2c_unregister_device(ps_bridge->page[i]);

exit:
	return ret;
}

static int ps8640_remove(struct i2c_client *client)
{
	struct ps8640 *ps_bridge = i2c_get_clientdata(client);
	int i;

	for (i = 1; i < 8; i++)
		if (ps_bridge->page[i])
			i2c_unregister_device(ps_bridge->page[i]);

	drm_bridge_remove(&ps_bridge->bridge);

	return 0;
}

static const struct i2c_device_id ps8640_i2c_table[] = {
	{"parade,ps8640", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, ps8640_i2c_table);

static const struct of_device_id ps8640_match[] = {
	{ .compatible = "parade,ps8640" },
	{},
};
MODULE_DEVICE_TABLE(of, ps8640_match);

static struct i2c_driver ps8640_driver = {
	.id_table = ps8640_i2c_table,
	.probe = ps8640_probe,
	.remove = ps8640_remove,
	.driver = {
		.name = "parade,ps8640",
		.of_match_table = ps8640_match,
	},
};
module_i2c_driver(ps8640_driver);

MODULE_AUTHOR("Jitao Shi <jitao.shi@mediatek.com>");
MODULE_AUTHOR("CK Hu <ck.hu@mediatek.com>");
MODULE_DESCRIPTION("PARADE ps8640 DSI-eDP converter driver");
MODULE_LICENSE("GPL v2");
