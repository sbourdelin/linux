/*
 * drivers/scsi/ufs/ufs-configfs.c
 *
 * Copyright (c) 2018, Qualcomm Technologies, Inc.
 *
 */

#include <linux/err.h>
#include <linux/string.h>
#include <asm/unaligned.h>
#include <linux/configfs.h>

#include "ufs.h"
#include "ufshcd.h"

struct ufs_hba *hba;

static ssize_t ufs_provision_show(struct config_item *item, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "provision_enabled = %x\n",
		hba->provision_enabled);
}

ssize_t ufshcd_desc_configfs_store(const char *buf, size_t count)
{
	struct ufs_config_descr *cfg = &hba->cfgs;
	char *strbuf;
	char *strbuf_copy;
	int desc_buf[count];
	int *pt;
	char *token;
	int i, ret;
	int value, commit = 0;
	int num_luns = 0;
	int KB_per_block = 4;

	/* reserve one byte for null termination */
	strbuf = kmalloc(count + 1, GFP_KERNEL);
	if (!strbuf)
		return -ENOMEM;

	strbuf_copy = strbuf;
	strlcpy(strbuf, buf, count + 1);
	memset(desc_buf, 0, count);

	/* Just return if bConfigDescrLock is already set */
	ret = ufshcd_query_attr(hba, UPIU_QUERY_OPCODE_READ_ATTR,
		QUERY_ATTR_IDN_CONF_DESC_LOCK, 0, 0, &cfg->bConfigDescrLock);
	if (ret) {
		dev_err(hba->dev, "%s: Failed reading bConfigDescrLock %d, cannot re-provision device!\n",
			__func__, ret);
		hba->provision_enabled = 0;
		goto out;
	}
	if (cfg->bConfigDescrLock == 1) {
		dev_err(hba->dev, "%s: bConfigDescrLock already set to %u, cannot re-provision device!\n",
		__func__, cfg->bConfigDescrLock);
		hba->provision_enabled = 0;
		goto out;
	}

	for (i = 0; i < count; i++) {
		token = strsep(&strbuf, " ");
		if (!token && i) {
			num_luns = desc_buf[i-1];
			dev_dbg(hba->dev, "%s: token %s, num_luns %d\n",
				__func__, token, num_luns);
			if (num_luns > 8) {
				dev_err(hba->dev, "%s: Invalid num_luns %d\n",
				__func__, num_luns);
				hba->provision_enabled = 0;
				goto out;
			}
			break;
		}

		ret = kstrtoint(token, 0, &value);
		if (ret) {
			dev_err(hba->dev, "%s: kstrtoint failed %d %s\n",
				__func__, ret, token);
			break;
		}
		desc_buf[i] = value;
		dev_dbg(hba->dev, " desc_buf[%d] 0x%x", i, desc_buf[i]);
	}

	/* Fill in the descriptors with parsed configuration data */
	pt = desc_buf;
	cfg->bNumberLU = *pt++;
	cfg->bBootEnable = *pt++;
	cfg->bDescrAccessEn = *pt++;
	cfg->bInitPowerMode = *pt++;
	cfg->bHighPriorityLUN = *pt++;
	cfg->bSecureRemovalType = *pt++;
	cfg->bInitActiveICCLevel = *pt++;
	cfg->wPeriodicRTCUpdate = *pt++;
	cfg->bConfigDescrLock = *pt++;
	dev_dbg(hba->dev, "%s: %u %u %u %u %u %u %u %u %u\n", __func__,
	cfg->bNumberLU, cfg->bBootEnable, cfg->bDescrAccessEn,
	cfg->bInitPowerMode, cfg->bHighPriorityLUN, cfg->bSecureRemovalType,
	cfg->bInitActiveICCLevel, cfg->wPeriodicRTCUpdate,
	cfg->bConfigDescrLock);

	for (i = 0; i < num_luns; i++) {
		cfg->unit[i].LUNum = *pt++;
		cfg->unit[i].bLUEnable = *pt++;
		cfg->unit[i].bBootLunID = *pt++;
		/* dNumAllocUnits = size_in_kb/KB_per_block */
		cfg->unit[i].dNumAllocUnits = (u32)(*pt++ / KB_per_block);
		cfg->unit[i].bDataReliability = *pt++;
		cfg->unit[i].bLUWriteProtect = *pt++;
		cfg->unit[i].bMemoryType = *pt++;
		cfg->unit[i].bLogicalBlockSize = *pt++;
		cfg->unit[i].bProvisioningType = *pt++;
		cfg->unit[i].wContextCapabilities = *pt++;
	}

	cfg->lun_to_grow = *pt++;
	commit = *pt++;
	cfg->num_luns = *pt;
	dev_dbg(hba->dev, "%s: lun_to_grow %u, commit %u num_luns %u\n",
		__func__, cfg->lun_to_grow, commit, cfg->num_luns);
	if (commit == 1) {
		ret = ufshcd_do_config_device(hba);
		if (!ret) {
			hba->provision_enabled = 1;
			dev_err(hba->dev,
			"%s: UFS Provisioning completed,num_luns %u, reboot now !\n",
			__func__, cfg->num_luns);
		}
	} else
		dev_err(hba->dev, "%s: Invalid commit %u\n", __func__, commit);
out:
	kfree(strbuf_copy);
	return count;
}

static ssize_t ufs_provision_store(struct config_item *item,
		const char *buf, size_t count)
{
	return ufshcd_desc_configfs_store(buf, count);
}

static struct configfs_attribute ufshcd_attr_provision = {
	.ca_name	= "ufs_provision",
	.ca_mode	= S_IRUGO | S_IWUGO,
	.ca_owner	= THIS_MODULE,
	.show		= ufs_provision_show,
	.store		= ufs_provision_store,
};

static struct configfs_attribute *ufshcd_attrs[] = {
	&ufshcd_attr_provision,
	NULL,
};

static struct config_item_type ufscfg_type = {
	.ct_attrs	= ufshcd_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct configfs_subsystem ufscfg_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "ufshcd",
			.ci_type = &ufscfg_type,
		},
	},
};

int ufshcd_configfs_init(struct ufs_hba *hba_ufs)
{
	int ret;
	struct configfs_subsystem *subsys = &ufscfg_subsys;
	hba = hba_ufs;

	config_group_init(&subsys->su_group);
	mutex_init(&subsys->su_mutex);
	ret = configfs_register_subsystem(subsys);
	if (ret) {
		pr_err("Error %d while registering subsystem %s\n",
		       ret,
		       subsys->su_group.cg_item.ci_namebuf);
	}
	return ret;
}

void ufshcd_configfs_exit(void)
{
	configfs_unregister_subsystem(&ufscfg_subsys);
}