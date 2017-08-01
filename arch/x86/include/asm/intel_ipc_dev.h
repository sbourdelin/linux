/*
 * intel_ipc_dev.h: IPC class device header file
 *
 * (C) Copyright 2017 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 *
 */

#include <linux/module.h>
#include <linux/device.h>

/* IPC channel type */
#define IPC_CHANNEL_IA_PMC			0
#define IPC_CHANNEL_IA_PUNIT			1
#define IPC_CHANNEL_PMC_PUNIT			2
#define IPC_CHANNEL_MAX				3

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

/**
 * struct intel_ipc_dev_cfg - IPC device config structure.
 *
 * IPC device drivers should provide following config options to
 * register new IPC device.
 *
 * @base:       IPC device memory resource start address.
 * @wrbuf_reg:  IPC device data write register address.
 * @rbuf_reg:   IPC device data read register address.
 * @sptr_reg:   IPC device source data pointer register address.
 * @dptr_reg: 	IPC device destination data pointer register address.
 * @status_reg: IPC command status register address.
 * @cmd_reg: 	IPC command register address.
 * @mode: 	IRQ/POLLING mode.
 * @irq:      	IPC device IRQ number.
 * @irqflags:   IPC device IRQ flags.
 * @chan_type:  IPC device channel type(PMC/PUNIT).
 * @msi:    	Enable/Disable MSI for IPC commands.
 *
 */
struct intel_ipc_dev_cfg {
	void __iomem *base;
	void __iomem *wrbuf_reg;
	void __iomem *rbuf_reg;
	void __iomem *sptr_reg;
	void __iomem *dptr_reg;
	void __iomem *status_reg;
	void __iomem *cmd_reg;
	int mode;
	int irq;
	int irqflags;
	int chan_type;
	bool use_msi;
};

/**
 * struct intel_ipc_dev_ops - IPC device ops structure.
 *
 * Call backs for IPC device specific operations.
 *
 * @err_code	: Status to error code conversion function.
 * @busy_check	: Check for IPC busy status.
 * @cmd_msi	: Enable MSI for IPC commands.
 *
 */
struct intel_ipc_dev_ops {
	int (*to_err_code)(int status);
	int (*busy_check)(int status);
	u32 (*enable_msi)(u32 cmd);

};

/**
 * struct intel_ipc_dev - Intel IPC device structure.
 *
 * Used with devm_intel_ipc_dev_create() to create new IPC device.
 *
 * @dev:        	IPC device object.
 * @cmd:  		Current IPC device command.
 * @cmd_complete:   	Command completion object.
 * @lock:   		Lock to protect IPC device structure.
 * @ops: 		IPC device ops pointer.
 * @cfg: 		IPC device cfg pointer.
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

int ipc_dev_simple_cmd(struct intel_ipc_dev *ipc_dev, u32 cmd);
int ipc_dev_raw_cmd(struct intel_ipc_dev *ipc_dev, u32 cmd, u8 *in,
		u32 inlen, u32 *out, u32 outlen, u32 dptr, u32 sptr);
#else

static inline struct intel_ipc_dev *devm_intel_ipc_dev_create(
		struct device *dev,
		const char *devname, struct intel_ipc_dev_cfg *cfg,
		struct intel_ipc_dev_ops *ops)
{
	return -EINVAL;
}

static inline int ipc_dev_simple_cmd(struct intel_ipc_dev *ipc_dev, u32 cmd)
{
	return -EINVAL;
}

static inline int ipc_dev_raw_cmd(struct intel_ipc_dev *ipc_dev, u32 cmd,
		u8 *in, u32 inlen, u32 *out, u32 outlen, u32 dptr, u32 sptr)
{
	return -EINVAL;
}

#endif
