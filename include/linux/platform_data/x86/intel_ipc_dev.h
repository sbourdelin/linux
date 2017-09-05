/*
 * Intel IPC class device header file.
 *
 * (C) Copyright 2017 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 *
 */

#ifndef INTEL_IPC_DEV_H
#define INTEL_IPC_DEV_H

#include <linux/module.h>
#include <linux/device.h>

/* IPC channel type */
#define IPC_CHANNEL_IA_PMC                      0
#define IPC_CHANNEL_IA_PUNIT                    1
#define IPC_CHANNEL_PMC_PUNIT                   2
#define IPC_CHANNEL_IA_SCU                      3
#define IPC_CHANNEL_MAX                         4

/* IPC return code */
#define IPC_DEV_ERR_NONE			0
#define IPC_DEV_ERR_CMD_NOT_SUPPORTED		1
#define IPC_DEV_ERR_CMD_NOT_SERVICED		2
#define IPC_DEV_ERR_UNABLE_TO_SERVICE		3
#define IPC_DEV_ERR_CMD_INVALID			4
#define IPC_DEV_ERR_CMD_FAILED			5
#define IPC_DEV_ERR_EMSECURITY			6
#define IPC_DEV_ERR_UNSIGNEDKERNEL		7

/* IPC mode */
#define IPC_DEV_MODE_IRQ			0
#define IPC_DEV_MODE_POLLING			1

/* IPC dev constants */
#define IPC_DEV_CMD_LOOP_CNT			3000000
#define IPC_DEV_CMD_TIMEOUT			3 * HZ
#define IPC_DEV_DATA_BUFFER_SIZE		16

struct intel_ipc_dev;
struct intel_ipc_raw_cmd;

/**
 * struct intel_ipc_dev_cfg - IPC device config structure.
 *
 * IPC device drivers uses the following config options to
 * register new IPC device.
 *
 * @cmd_regs            : IPC device command base regmap.
 * @data_regs           : IPC device data base regmap.
 * @wrbuf_reg           : IPC device data write register address.
 * @rbuf_reg            : IPC device data read register address.
 * @sptr_reg            : IPC device source data pointer register address.
 * @dptr_reg            : IPC device destination data pointer register
 *                        address.
 * @status_reg          : IPC command status register address.
 * @cmd_reg             : IPC command register address.
 * @mode                : IRQ/POLLING mode.
 * @irq                 : IPC device IRQ number.
 * @irqflags            : IPC device IRQ flags.
 * @chan_type           : IPC device channel type(PMC/PUNIT).
 * @msi                 : Enable/Disable MSI for IPC commands.
 * @support_dptr        : Support DPTR update.
 * @support_sptr        : Support SPTR update.
 *
 */
struct intel_ipc_dev_cfg {
	struct regmap *cmd_regs;
	struct regmap *data_regs;
	unsigned int wrbuf_reg;
	unsigned int rbuf_reg;
	unsigned int sptr_reg;
	unsigned int dptr_reg;
	unsigned int status_reg;
	unsigned int cmd_reg;
	int mode;
	int irq;
	int irqflags;
	int chan_type;
	bool use_msi;
	bool support_dptr;
	bool support_sptr;
};

/**
 * struct intel_ipc_dev_ops - IPC device ops structure.
 *
 * Call backs for IPC device specific operations.
 *
 * @to_err_code         : Status to error code conversion function.
 * @busy_check          : Check for IPC busy status.
 * @enable_msi          : Enable MSI for IPC commands.
 * @pre_simple_cmd_fn   : Custom pre-processing function for
 *                        ipc_dev_simple_cmd()
 * @pre_cmd_fn          : Custom pre-processing function for
 *                        ipc_dev_cmd()
 * @pre_raw_cmd_fn      : Custom pre-processing function for
 *                        ipc_dev_raw_cmd()
 *
 */
struct intel_ipc_dev_ops {
	int (*to_err_code)(int status);
	int (*busy_check)(int status);
	u32 (*enable_msi)(u32 cmd);
	int (*pre_simple_cmd_fn)(u32 *cmd_list, u32 cmdlen);
	int (*pre_cmd_fn)(u32 *cmd_list, u32 cmdlen, u32 *in, u32 inlen,
			u32 *out, u32 outlen);
	int (*pre_raw_cmd_fn)(u32 *cmd_list, u32 cmdlen, u8 *in, u32 inlen,
			u32 *out, u32 outlen, u32 dptr, u32 sptr);
};

/**
 * struct intel_ipc_dev - Intel IPC device structure.
 *
 * Used with devm_intel_ipc_dev_create() to create new IPC device.
 *
 * @dev                 : IPC device object.
 * @cmd                 : Current IPC device command.
 * @cmd_complete        : Command completion object.
 * @lock                : Lock to protect IPC device structure.
 * @ops                 : IPC device ops pointer.
 * @cfg                 : IPC device cfg pointer.
 *
 */
struct intel_ipc_dev {
	struct device dev;
	int cmd;
	struct completion cmd_complete;
	struct mutex lock;
	struct intel_ipc_dev_ops *ops;
	struct intel_ipc_dev_cfg *cfg;
};

#if IS_ENABLED(CONFIG_INTEL_IPC_DEV)

/* API to create new IPC device */
struct intel_ipc_dev *devm_intel_ipc_dev_create(struct device *dev,
		const char *devname, struct intel_ipc_dev_cfg *cfg,
		struct intel_ipc_dev_ops *ops);

int ipc_dev_simple_cmd(struct intel_ipc_dev *ipc_dev, u32 *cmd_list,
		u32 cmdlen);
int ipc_dev_cmd(struct intel_ipc_dev *ipc_dev, u32 *cmd_list, u32 cmdlen,
		u32 *in, u32 inlen, u32 *out, u32 outlen);
int ipc_dev_raw_cmd(struct intel_ipc_dev *ipc_dev, u32 *cmd_list, u32 cmdlen,
		u8 *in, u32 inlen, u32 *out, u32 outlen, u32 dptr, u32 sptr);
struct intel_ipc_dev *intel_ipc_dev_get(const char *dev_name);
#else

static inline struct intel_ipc_dev *devm_intel_ipc_dev_create(
		struct device *dev,
		const char *devname, struct intel_ipc_dev_cfg *cfg,
		struct intel_ipc_dev_ops *ops)
{
	return -EINVAL;
}

static inline int ipc_dev_simple_cmd(struct intel_ipc_dev *ipc_dev,
		u32 *cmd_list, u32 cmdlen)
{
	return -EINVAL;
}

static int ipc_dev_cmd(struct intel_ipc_dev *ipc_dev, u32 *cmd_list,
		u32 cmdlen, u32 *in, u32 inlen, u32 *out, u32 outlen)
{
	return -EINVAL;
}

static inline int ipc_dev_raw_cmd(struct intel_ipc_dev *ipc_dev, u32 *cmd_list,
		u32 cmdlen, u8 *in, u32 inlen, u32 *out, u32 outlen,
		u32 dptr, u32 sptr);
{
	return -EINVAL;
}

static inline struct intel_ipc_dev *intel_ipc_dev_get(const char *dev_name)
{
	return NULL;
}
#endif /* CONFIG_INTEL_IPC_DEV */
#endif /* INTEL_IPC_DEV_H */
