/*
 * Copyright (c) 2015 Linaro Ltd.
 * Copyright (c) 2015 Hisilicon Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifndef _HISI_SAS_H_
#define _HISI_SAS_H_

#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <scsi/libsas.h>

#define DRV_NAME "hisi_sas"
#define DRV_VERSION "v1.0"

#define HISI_SAS_MAX_PHYS	9
#define HISI_SAS_MAX_ITCT_ENTRIES 4096
#define HISI_SAS_MAX_DEVICES HISI_SAS_MAX_ITCT_ENTRIES
#define HISI_SAS_COMMAND_ENTRIES 8192


struct hisi_sas_phy {
	struct hisi_sas_port	*port;
	struct asd_sas_phy	sas_phy;
};

struct hisi_sas_port {
	struct asd_sas_port	sas_port;
};

struct hisi_hba {
	spinlock_t	lock;

	struct platform_device *pdev;


	u8 sas_addr[SAS_ADDR_SIZE];

	int	n_phy;
	/* SCSI/SAS glue */
	struct sas_ha_struct sha;
	struct Scsi_Host *shost;
	struct hisi_sas_phy phy[HISI_SAS_MAX_PHYS];
	struct hisi_sas_port port[HISI_SAS_MAX_PHYS];
};

#define HISI_SAS_SGE_PAGE_CNT SCSI_MAX_SG_SEGMENTS
#endif
