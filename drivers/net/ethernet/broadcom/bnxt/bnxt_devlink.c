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
	{DEVLINK_ATTR_MAX_NUM_PF_MSIX_VECT, BNXT_DRV_PF,
		BNXT_DRV_APPL_SHARED, 10, 108},
	{DEVLINK_ATTR_IGNORE_ARI_CAPABILITY, BNXT_DRV_PF,
		BNXT_DRV_APPL_SHARED, 1, 164},
	{DEVLINK_ATTR_PME_CAPABILITY_ENABLED, BNXT_DRV_PF,
		BNXT_DRV_APPL_SHARED, 1, 166},
	{DEVLINK_ATTR_LLDP_NEAREST_BRIDGE_ENABLED, BNXT_DRV_PF,
		BNXT_DRV_APPL_SHARED, 1, 269},
	{DEVLINK_ATTR_LLDP_NEAREST_NONTPMR_BRIDGE_ENABLED, BNXT_DRV_PF,
		BNXT_DRV_APPL_SHARED, 1, 270},
	{DEVLINK_ATTR_SECURE_NIC_ENABLED, BNXT_DRV_PF,
		BNXT_DRV_APPL_SHARED, 1, 162},
	{DEVLINK_ATTR_PHY_SELECT, BNXT_DRV_PF,
		BNXT_DRV_APPL_SHARED, 1, 329},
	{DEVLINK_ATTR_SRIOV_ENABLED, BNXT_DRV_PF,
		BNXT_DRV_APPL_SHARED, 1, 401},

	{DEVLINK_ATTR_MBA_ENABLED, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 1, 351},
	{DEVLINK_ATTR_MBA_BOOT_TYPE, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 2, 352},
	{DEVLINK_ATTR_MBA_DELAY_TIME, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 4, 353},
	{DEVLINK_ATTR_MBA_SETUP_HOT_KEY, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 1, 354},
	{DEVLINK_ATTR_MBA_HIDE_SETUP_PROMPT, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 1, 355},
	{DEVLINK_ATTR_MBA_VLAN_TAG, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 16, 357},
	{DEVLINK_ATTR_MBA_VLAN_ENABLED, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 1, 358},
	{DEVLINK_ATTR_MBA_LINK_SPEED, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 4, 359},
	{DEVLINK_ATTR_MBA_BOOT_RETRY_COUNT, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 3, 360},
	{DEVLINK_ATTR_MBA_BOOT_PROTOCOL, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 3, 361},
	{DEVLINK_ATTR_NUM_VF_PER_PF, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 8, 404},
	{DEVLINK_ATTR_MSIX_VECTORS_PER_VF, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 10, 406},
	{DEVLINK_ATTR_NPAR_BW_RESERVATION, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 10, 501},
	{DEVLINK_ATTR_NPAR_BW_LIMIT, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 10, 502},
	{DEVLINK_ATTR_RDMA_ENABLED, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 1, 506},
	{DEVLINK_ATTR_NPAR_BW_IN_PERCENT, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 1, 507},
	{DEVLINK_ATTR_NPAR_BW_RESERVATION_VALID, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 1, 508},
	{DEVLINK_ATTR_NPAR_BW_LIMIT_VALID, BNXT_DRV_PF,
		BNXT_DRV_APPL_FUNCTION, 1, 509},

	{DEVLINK_ATTR_MAGIC_PACKET_WOL_ENABLED, BNXT_DRV_PF,
		BNXT_DRV_APPL_PORT, 1, 152},
	{DEVLINK_ATTR_DCBX_MODE, BNXT_DRV_PF,
		BNXT_DRV_APPL_PORT, 4, 155},
	{DEVLINK_ATTR_MULTIFUNC_MODE, BNXT_DRV_PF,
		BNXT_DRV_APPL_PORT, 5, 157},
	{DEVLINK_ATTR_PRE_OS_LINK_SPEED_D0, BNXT_DRV_PF,
		BNXT_DRV_APPL_PORT, 4, 205},
	{DEVLINK_ATTR_EEE_PWR_SAVE_ENABLED, BNXT_DRV_PF,
		BNXT_DRV_APPL_PORT, 1, 208},
	{DEVLINK_ATTR_PRE_OS_LINK_SPEED_D3, BNXT_DRV_PF,
		BNXT_DRV_APPL_PORT, 4, 210},
	{DEVLINK_ATTR_MEDIA_AUTO_DETECT, BNXT_DRV_PF,
		BNXT_DRV_APPL_PORT, 1, 213},
	{DEVLINK_ATTR_AUTONEG_PROTOCOL, BNXT_DRV_PF,
		BNXT_DRV_APPL_PORT, 8, 312},
	{DEVLINK_ATTR_NPAR_NUM_PARTITIONS_PER_PORT, BNXT_DRV_PF,
		BNXT_DRV_APPL_PORT, 8, 503},
};

#define BNXT_NUM_DRV_CFGPARAM ARRAY_SIZE(bnxt_drv_cfgparam_list)

static int bnxt_dl_eswitch_mode_get(struct devlink *devlink, u16 *mode)
{
	struct bnxt *bp = bnxt_get_bp_from_dl(devlink);

	*mode = bp->eswitch_mode;
	return 0;
}

static int bnxt_dl_eswitch_mode_set(struct devlink *devlink, u16 mode)
{
	struct bnxt *bp = bnxt_get_bp_from_dl(devlink);
	int rc = 0;

	mutex_lock(&bp->sriov_lock);
	if (bp->eswitch_mode == mode) {
		netdev_info(bp->dev, "already in %s eswitch mode",
			    mode == DEVLINK_ESWITCH_MODE_LEGACY ?
			    "legacy" : "switchdev");
		rc = -EINVAL;
		goto done;
	}

	switch (mode) {
	case DEVLINK_ESWITCH_MODE_LEGACY:
		bnxt_vf_reps_destroy(bp);
		break;

	case DEVLINK_ESWITCH_MODE_SWITCHDEV:
		if (pci_num_vf(bp->pdev) == 0) {
			netdev_info(bp->dev,
				    "Enable VFs before setting switchdev mode");
			rc = -EPERM;
			goto done;
		}
		rc = bnxt_vf_reps_create(bp);
		break;

	default:
		rc = -EINVAL;
		goto done;
	}
done:
	mutex_unlock(&bp->sriov_lock);
	return rc;
}

static int bnxt_nvm_read(struct bnxt *bp, int nvm_param, int idx,
			 void *buf, int size)
{
	struct hwrm_nvm_get_variable_input req = {0};
	void *dest_data_addr = NULL;
	dma_addr_t dest_data_dma_addr;
	int rc;
	int bytesize;

	bytesize = (size + 7) / 8;
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

	memcpy(buf, dest_data_addr, size);

	dma_free_coherent(&bp->pdev->dev, bytesize, dest_data_addr,
			  dest_data_dma_addr);

	return rc;
}

static int bnxt_nvm_write(struct bnxt *bp, int nvm_param, int idx,
			  const void *buf, int size)
{
	struct hwrm_nvm_set_variable_input req = {0};
	void *src_data_addr = NULL;
	dma_addr_t src_data_dma_addr;
	int rc;
	int bytesize;

	bytesize = (size + 7) / 8;

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

static int bnxt_dl_config_set(struct devlink *devlink,
			      enum devlink_attr attr, u32 value,
			      u8 *restart_reqd)
{
	struct bnxt *bp = bnxt_get_bp_from_dl(devlink);
	int i;
	int idx = 0;
	void *data;
	int ret = 0;
	u32 bytesize;
	struct bnxt_drv_cfgparam *entry;

	*restart_reqd = 0;

	/* Find parameter in table */
	for (i = 0; i < BNXT_NUM_DRV_CFGPARAM; i++) {
		if (attr == bnxt_drv_cfgparam_list[i].attr) {
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
		else
			idx = bp->vf.fw_fid - BNXT_FIRST_VF_FID;
	}

	bytesize = (entry->bitlength + 7) / 8;
	data = kmalloc(bytesize, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if (bytesize == 1) {
		u8 val8 = (value & 0xff);

		memcpy(data, &val8, sizeof(u8));
	} else if (bytesize == 2) {
		u16 val16 = (value & 0xffff);

		memcpy(data, &val16, sizeof(u16));
	} else {
		memcpy(data, &value, sizeof(u32));
	}

	ret = bnxt_nvm_write(bp, entry->nvm_param, idx, data,
			     entry->bitlength);

	/* Restart required for all nvm parameter writes */
	*restart_reqd = 1;

	kfree(data);

	return ret;
}

static int bnxt_dl_config_get(struct devlink *devlink,
			      enum devlink_attr attr, u32 *value)
{
	struct bnxt *bp = bnxt_get_bp_from_dl(devlink);

	int i;
	int idx = 0;
	void *data;
	int ret = 0;
	u32 bytesize;
	struct bnxt_drv_cfgparam *entry;

	/* Find parameter in table */
	for (i = 0; i < BNXT_NUM_DRV_CFGPARAM; i++) {
		if (attr == bnxt_drv_cfgparam_list[i].attr) {
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
		else
			idx = bp->vf.fw_fid - BNXT_FIRST_VF_FID;
	}

	/* Allocate space, retrieve value, and copy to result */
	bytesize = (entry->bitlength + 7) / 8;
	data = kmalloc(bytesize, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	ret = bnxt_nvm_read(bp, entry->nvm_param, idx, data, entry->bitlength);

	if (ret) {
		kfree(data);
		return ret;
	}

	if (bytesize == 1) {
		u8 val;

		memcpy(&val, data, sizeof(u8));
		*value = val;
	} else if (bytesize == 2) {
		u16 val;

		memcpy(&val, data, sizeof(u16));
		*value = val;
	} else {
		u32 val;

		memcpy(&val, data, sizeof(u32));
		*value = val;
	}

	kfree(data);

	return 0;
}

static struct devlink_ops bnxt_dl_ops = {
	.eswitch_mode_set = bnxt_dl_eswitch_mode_set,
	.eswitch_mode_get = bnxt_dl_eswitch_mode_get,
	.config_get = bnxt_dl_config_get,
	.config_set = bnxt_dl_config_set,
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
