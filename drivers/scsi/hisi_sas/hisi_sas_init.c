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

#include "hisi_sas.h"
static struct scsi_transport_template *hisi_sas_stt;

static struct scsi_host_template hisi_sas_sht = {
	.module			= THIS_MODULE,
	.name			= DRV_NAME,
	.queuecommand		= sas_queuecommand,
	.target_alloc		= sas_target_alloc,
	.slave_configure	= sas_slave_configure,
	.change_queue_depth	= sas_change_queue_depth,
	.bios_param		= sas_bios_param,
	.can_queue		= 1,
	.cmd_per_lun		= 1,
	.this_id		= -1,
	.sg_tablesize		= SG_ALL,
	.max_sectors		= SCSI_DEFAULT_MAX_SECTORS,
	.use_clustering		= ENABLE_CLUSTERING,
	.eh_device_reset_handler = sas_eh_device_reset_handler,
	.eh_bus_reset_handler	= sas_eh_bus_reset_handler,
	.target_destroy		= sas_target_destroy,
	.ioctl			= sas_ioctl,
};

static struct sas_domain_function_template hisi_sas_transport_ops = {
};


static const struct of_device_id sas_of_match[] = {
	{ .compatible = "hisilicon,sas-controller-v1",},
	{},
};
MODULE_DEVICE_TABLE(of, sas_of_match);

static struct hisi_hba *hisi_sas_hba_alloc(
			struct platform_device *pdev,
			struct Scsi_Host *shost,
			struct device_node *np)
{
	int interrupt_count, interrupt_cells;
	struct hisi_hba *hisi_hba;

	hisi_hba = devm_kzalloc(&pdev->dev, sizeof(*hisi_hba), GFP_KERNEL);
	if (!hisi_hba)
		goto err_out;

	hisi_hba->pdev = pdev;
	hisi_hba->shost = shost;

	return hisi_hba;
err_out:
	dev_err(&pdev->dev, "hba alloc failed\n");
	return NULL;
}

static int hisi_sas_probe(struct platform_device *pdev)
{
	struct Scsi_Host *shost;
	struct hisi_hba *hisi_hba;
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct asd_sas_phy **arr_phy;
	struct asd_sas_port **arr_port;
	struct sas_ha_struct *sha;
	int rc, phy_nr, port_nr, i;

	shost = scsi_host_alloc(&hisi_sas_sht, sizeof(void *));
	if (!shost)
		return -ENOMEM;

	hisi_hba = hisi_sas_hba_alloc(pdev, shost, np);
	if (!hisi_hba) {
		rc = -ENOMEM;
		goto err_out_ha;
	}

	sha = SHOST_TO_SAS_HA(shost) = &hisi_hba->sha;
	platform_set_drvdata(pdev, sha);

	phy_nr = port_nr = HISI_SAS_MAX_PHYS;

	arr_phy = devm_kcalloc(dev, phy_nr, sizeof(void *), GFP_KERNEL);
	arr_port = devm_kcalloc(dev, port_nr, sizeof(void *), GFP_KERNEL);
	if (!arr_phy || !arr_port)
		return -ENOMEM;

	sha->sas_phy = arr_phy;
	sha->sas_port = arr_port;
	sha->core.shost = shost;
	sha->lldd_ha = hisi_hba;

	shost->transportt = hisi_sas_stt;
	shost->max_id = HISI_SAS_MAX_DEVICES;
	shost->max_lun = ~0;
	shost->max_channel = 1;
	shost->max_cmd_len = 16;
	shost->sg_tablesize = min_t(u16, SG_ALL, HISI_SAS_SGE_PAGE_CNT);
	shost->can_queue = HISI_SAS_COMMAND_ENTRIES;
	shost->cmd_per_lun = HISI_SAS_COMMAND_ENTRIES;

	sha->sas_ha_name = DRV_NAME;
	sha->dev = &hisi_hba->pdev->dev;
	sha->lldd_module = THIS_MODULE;
	sha->sas_addr = &hisi_hba->sas_addr[0];
	sha->num_phys = hisi_hba->n_phy;
	sha->core.shost = hisi_hba->shost;

	for (i = 0; i < hisi_hba->n_phy; i++) {
		sha->sas_phy[i] = &hisi_hba->phy[i].sas_phy;
		sha->sas_port[i] = &hisi_hba->port[i].sas_port;
	}
	rc = scsi_add_host(shost, &pdev->dev);
	if (rc)
		goto err_out_ha;

	rc = sas_register_ha(SHOST_TO_SAS_HA(shost));
	if (rc)
		goto err_out_register_ha;

	return 0;

err_out_register_ha:
	scsi_remove_host(shost);
err_out_ha:
	kfree(shost);
	return rc;
}

static int hisi_sas_remove(struct platform_device *pdev)
{
	struct sas_ha_struct *sha = platform_get_drvdata(pdev);

	sas_unregister_ha(sha);
	sas_remove_host(sha->core.shost);
	scsi_remove_host(sha->core.shost);

	return 0;
}

static struct platform_driver hisi_sas_driver = {
	.probe = hisi_sas_probe,
	.remove = hisi_sas_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = sas_of_match,
	},
};

static __init int hisi_sas_init(void)
{
	pr_info("hisi_sas: driver version %s\n", DRV_VERSION);

	hisi_sas_stt = sas_domain_attach_transport(&hisi_sas_transport_ops);
	if (!hisi_sas_stt)
		return -ENOMEM;

	return platform_driver_register(&hisi_sas_driver);
}

static __exit void hisi_sas_exit(void)
{
	platform_driver_unregister(&hisi_sas_driver);
	sas_release_transport(hisi_sas_stt);
}

module_init(hisi_sas_init);
module_exit(hisi_sas_exit);

MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("John Garry <john.garry@huawei.com>");
MODULE_DESCRIPTION("HISILICON SAS controller driver");
MODULE_ALIAS("platform:" DRV_NAME);
