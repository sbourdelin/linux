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

static const struct devlink_ops bnxt_dl_ops = {
	.eswitch_mode_set = bnxt_dl_eswitch_mode_set,
	.eswitch_mode_get = bnxt_dl_eswitch_mode_get,
};

int bnxt_dl_register(struct bnxt *bp)
{
	struct devlink *dl;
	int rc;

	if (!pci_find_ext_capability(bp->pdev, PCI_EXT_CAP_ID_SRIOV))
		return 0;

	if (bp->hwrm_spec_code < 0x10800) {
		netdev_warn(bp->dev, "Firmware does not support SR-IOV E-Switch SWITCHDEV mode.\n");
		return -ENOTSUPP;
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
