/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2017 Broadcom Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#include <linux/pci.h>
#include <linux/netdevice.h>
#include "bnxt_hsi.h"
#include "bnxt.h"
#include "bnxt_vfr.h"
#include "bnxt_devlink.h"

struct bnxt_drv_cfgparam bnxt_drv_cfgparam_list[] = {
};

#define BNXT_NUM_DRV_CFGPARAM ARRAY_SIZE(bnxt_drv_cfgparam_list)

static int bnxt_nvm_read(struct bnxt *bp, int nvm_param, int idx,
			 void *buf, int size)
{
	struct hwrm_nvm_get_variable_input req = {0};
	dma_addr_t dest_data_dma_addr;
	void *dest_data_addr = NULL;
	int bytesize;
	int rc;

	bytesize = (size + 7) / BITS_PER_BYTE;
	dest_data_addr = dma_alloc_coherent(&bp->pdev->dev, bytesize,
					    &dest_data_dma_addr, GFP_KERNEL);
	if (!dest_data_addr) {
		netdev_err(bp->dev, "dma_alloc_coherent failure\n");
		return -ENOMEM;
	}

	bnxt_hwrm_cmd_hdr_init(bp, &req, HWRM_NVM_GET_VARIABLE, -1, -1);
	req.dest_data_addr = cpu_to_le64(dest_data_dma_addr);
	req.data_len = cpu_to_le16(size);
	req.option_num = cpu_to_le16(nvm_param);
	req.index_0 = cpu_to_le16(idx);
	if (idx != 0)
		req.dimensions = cpu_to_le16(1);

	rc = _hwrm_send_message(bp, &req, sizeof(req), HWRM_CMD_TIMEOUT);

	memcpy(buf, dest_data_addr, bytesize);

	dma_free_coherent(&bp->pdev->dev, bytesize, dest_data_addr,
			  dest_data_dma_addr);

	return rc;
}

static int bnxt_nvm_write(struct bnxt *bp, int nvm_param, int idx,
			  const void *buf, int size)
{
	struct hwrm_nvm_set_variable_input req = {0};
	dma_addr_t src_data_dma_addr;
	void *src_data_addr = NULL;
	int bytesize;
	int rc;

	bytesize = (size + 7) / BITS_PER_BYTE;

	src_data_addr = dma_alloc_coherent(&bp->pdev->dev, bytesize,
					   &src_data_dma_addr, GFP_KERNEL);
	if (!src_data_addr) {
		netdev_err(bp->dev, "dma_alloc_coherent failure\n");
		return -ENOMEM;
	}

	memcpy(src_data_addr, buf, bytesize);

	bnxt_hwrm_cmd_hdr_init(bp, &req, HWRM_NVM_SET_VARIABLE, -1, -1);
	req.src_data_addr = cpu_to_le64(src_data_dma_addr);
	req.data_len = cpu_to_le16(size);
	req.option_num = cpu_to_le16(nvm_param);
	req.index_0 = cpu_to_le16(idx);
	if (idx != 0)
		req.dimensions = cpu_to_le16(1);

	rc = _hwrm_send_message(bp, &req, sizeof(req), HWRM_CMD_TIMEOUT);

	dma_free_coherent(&bp->pdev->dev, bytesize, src_data_addr,
			  src_data_dma_addr);

	return 0;
}

static int bnxt_dl_perm_config_set(struct devlink *devlink,
				   enum devlink_perm_config_param param,
				   u8 type, void *value, u8 *restart_reqd)
{
	struct bnxt *bp = bnxt_get_bp_from_dl(devlink);
	struct bnxt_drv_cfgparam *entry;
	int idx = 0;
	int ret = 0;
	u32 bytesize;
	u32 val32;
	u16 val16;
	u8 val8;
	int i;

	*restart_reqd = 0;

	/* Find parameter in table */
	for (i = 0; i < BNXT_NUM_DRV_CFGPARAM; i++) {
		if (param == bnxt_drv_cfgparam_list[i].param) {
			entry = &bnxt_drv_cfgparam_list[i];
			break;
		}
	}

	/* Not found */
	if (i == BNXT_NUM_DRV_CFGPARAM)
		return -EINVAL;

	/* Check to see if this func type can access variable */
	if (BNXT_PF(bp) && !(entry->func & BNXT_DRV_PF))
		return -EOPNOTSUPP;
	if (BNXT_VF(bp) && !(entry->func & BNXT_DRV_VF))
		return -EOPNOTSUPP;

	/* If parameter is per port or function, compute index */
	if (entry->appl == BNXT_DRV_APPL_PORT) {
		idx = bp->pf.port_id;
	} else if (entry->appl == BNXT_DRV_APPL_FUNCTION) {
		if (BNXT_PF(bp))
			idx = bp->pf.fw_fid - BNXT_FIRST_PF_FID;
	}

	bytesize = (entry->bitlength + 7) / BITS_PER_BYTE;

	/* If passed in type matches bytesize, pass value directly */
	if ((bytesize == 1 && type == NLA_U8) ||
	    (bytesize == 2 && type == NLA_U16) ||
	    (bytesize == 4 && type == NLA_U32)) {
		ret = bnxt_nvm_write(bp, entry->nvm_param, idx, value,
				     entry->bitlength);
	} else {
		/* Otherwise copy value to u32, then to driver type */
		if (type == NLA_U8) {
			memcpy(&val8, value, sizeof(val8));
			val32 = val8;
		} else if (type == NLA_U16) {
			memcpy(&val16, value, sizeof(val16));
			val32 = val16;
		} else if (type == NLA_U32) {
			memcpy(&val32, value, sizeof(val32));
		} else {
			/* Unsupported type */
			return -EINVAL;
		}

		if (bytesize == 1) {
			val8 = val32;
			ret = bnxt_nvm_write(bp, entry->nvm_param, idx, &val8,
					     entry->bitlength);
		} else if (bytesize == 2) {
			val16 = val32;
			ret = bnxt_nvm_write(bp, entry->nvm_param, idx, &val16,
					     entry->bitlength);
		} else {
			ret = bnxt_nvm_write(bp, entry->nvm_param, idx, &val32,
					     entry->bitlength);
		}
	}

	/* Restart required for all nvm parameter writes */
	*restart_reqd = 1;

	return ret;
}

static int bnxt_dl_perm_config_get(struct devlink *devlink,
				   enum devlink_perm_config_param param,
				   u8 type, void *value)
{
	struct bnxt *bp = bnxt_get_bp_from_dl(devlink);
	struct bnxt_drv_cfgparam *entry;
	u32 bytesize;
	int idx = 0;
	int err = 0;
	void *data;
	u32 val32;
	u16 val16;
	u8 val8;
	int i;

	/* Find parameter in table */
	for (i = 0; i < BNXT_NUM_DRV_CFGPARAM; i++) {
		if (param == bnxt_drv_cfgparam_list[i].param) {
			entry = &bnxt_drv_cfgparam_list[i];
			break;
		}
	}

	/* Not found */
	if (i == BNXT_NUM_DRV_CFGPARAM)
		return -EINVAL;

	/* Check to see if this func type can access variable */
	if (BNXT_PF(bp) && !(entry->func & BNXT_DRV_PF))
		return -EOPNOTSUPP;
	if (BNXT_VF(bp) && !(entry->func & BNXT_DRV_VF))
		return -EOPNOTSUPP;

	/* If parameter is per port or function, compute index */
	if (entry->appl == BNXT_DRV_APPL_PORT) {
		idx = bp->pf.port_id;
	} else if (entry->appl == BNXT_DRV_APPL_FUNCTION) {
		if (BNXT_PF(bp))
			idx = bp->pf.fw_fid - BNXT_FIRST_PF_FID;
	}

	/* Allocate space, retrieve value, and copy to result */
	bytesize = (entry->bitlength + 7) / BITS_PER_BYTE;
	data = kmalloc(bytesize, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	err = bnxt_nvm_read(bp, entry->nvm_param, idx, data, entry->bitlength);
	if (err)
		goto err_data;

	/* if bytesize matches type requested, just copy data */
	if ((bytesize == 1 && type == NLA_U8) ||
	    (bytesize == 2 && type == NLA_U16) ||
	    (bytesize == 4 && type == NLA_U32)) {
		memcpy(value, data, bytesize);
	} else {
		/* Otherwise copy data to u32, then to requested type */
		if (bytesize == 1) {
			memcpy(&val8, data, sizeof(val8));
			val32 = val8;
		} else if (bytesize == 2) {
			memcpy(&val16, data, sizeof(val16));
			val32 = val16;
		} else {
			memcpy(&val32, data, sizeof(val32));
		}

		if (type == NLA_U8) {
			val8 = val32;
			memcpy(value, &val8, sizeof(val8));
		} else if (type == NLA_U16) {
			val16 = val32;
			memcpy(value, &val16, sizeof(val16));
		} else if (type == NLA_U32) {
			memcpy(value, &val32, sizeof(val32));
		} else {
			/* Unsupported type */
			err = -EINVAL;
		}
	}

err_data:
	kfree(data);

	return err;
}

static struct devlink_ops bnxt_dl_ops = {
#ifdef CONFIG_BNXT_SRIOV
	.eswitch_mode_set = bnxt_dl_eswitch_mode_set,
	.eswitch_mode_get = bnxt_dl_eswitch_mode_get,
#endif /* CONFIG_BNXT_SRIOV */
	.perm_config_get = bnxt_dl_perm_config_get,
	.perm_config_set = bnxt_dl_perm_config_set,
};

int bnxt_dl_register(struct bnxt *bp)
{
	struct devlink *dl;
	int rc;

	if ((!pci_find_ext_capability(bp->pdev, PCI_EXT_CAP_ID_SRIOV)) ||
	    bp->hwrm_spec_code < 0x10800) {
		/* eswitch switchdev mode not supported */
		bnxt_dl_ops.eswitch_mode_set = NULL;
		bnxt_dl_ops.eswitch_mode_get = NULL;
		netdev_warn(bp->dev, "Firmware does not support SR-IOV E-Switch SWITCHDEV mode.\n");
	}

	dl = devlink_alloc(&bnxt_dl_ops, sizeof(struct bnxt_dl));
	if (!dl) {
		netdev_warn(bp->dev, "devlink_alloc failed");
		return -ENOMEM;
	}

	bnxt_link_bp_to_dl(bp, dl);
	bp->eswitch_mode = DEVLINK_ESWITCH_MODE_LEGACY;
	rc = devlink_register(dl, &bp->pdev->dev);
	if (rc) {
		bnxt_link_bp_to_dl(bp, NULL);
		devlink_free(dl);
		netdev_warn(bp->dev, "devlink_register failed. rc=%d", rc);
		return rc;
	}

	return 0;
}

void bnxt_dl_unregister(struct bnxt *bp)
{
	struct devlink *dl = bp->dl;

	if (!dl)
		return;

	devlink_unregister(dl);
	devlink_free(dl);
}
