/*
 * Soundwire Intel Driver
 * Copyright (c) 2016-17, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/soundwire/soundwire.h>
#include <linux/soundwire/sdw_intel.h>
#include "sdw_intel_shim.h"

#define SDW_ISHIM_BASE				0x2C000
#define SDW_IALH_BASE				0x2C800
#define SDW_ILINK_BASE				0x30000
#define SDW_ILINK_SIZE				0x10000

/* Intel SHIM Registers Definition */
#define SDW_ISHIM_LCAP				0x0
#define SDW_ISHIM_LCTL				0x4
#define SDW_ISHIM_IPPTR				0x8
#define SDW_ISHIM_SYNC				0xC

#define SDW_ISHIM_CTLSCAP(x)			(0x010 + 0x60 * x)
#define SDW_ISHIM_CTLS0CM(x)			(0x012 + 0x60 * x)
#define SDW_ISHIM_CTLS1CM(x)			(0x014 + 0x60 * x)
#define SDW_ISHIM_CTLS2CM(x)			(0x016 + 0x60 * x)
#define SDW_ISHIM_CTLS3CM(x)			(0x018 + 0x60 * x)
#define SDW_ISHIM_PCMSCAP(x)			(0x020 + 0x60 * x)

#define SDW_ISHIM_PCMSYCHM(x, y)		(0x022 + (0x60 * x) + (0x2 * y))
#define SDW_ISHIM_PCMSYCHC(x, y)		(0x042 + (0x60 * x) + (0x2 * y))
#define SDW_ISHIM_PDMSCAP(x)			(0x062 + 0x60 * x)
#define SDW_ISHIM_IOCTL(x)			(0x06C + 0x60 * x)
#define SDW_ISHIM_CTMCTL(x)			(0x06E + 0x60 * x)
#define SDW_ISHIM_WAKEEN			0x190
#define SDW_ISHIM_WAKESTS			0x192

#define SDW_ISHIM_LCTL_SPA			BIT(0)
#define SDW_ISHIM_LCTL_CPA			BIT(8)

#define SDW_ISHIM_SYNC_SYNCPRD_VAL		0x176F
#define SDW_ISHIM_SYNC_SYNCPRD			GENMASK(14, 0)
#define SDW_ISHIM_SYNC_SYNCCPU			BIT(15)
#define SDW_ISHIM_SYNC_CMDSYNC_MASK		GENMASK(19, 16)
#define SDW_ISHIM_SYNC_CMDSYNC			BIT(16)
#define SDW_ISHIM_SYNC_SYNCGO			BIT(24)

#define SDW_ISHIM_PCMSCAP_ISS			GENMASK(3, 0)
#define SDW_ISHIM_PCMSCAP_OSS			GENMASK(7, 4)
#define SDW_ISHIM_PCMSCAP_BSS			GENMASK(12, 8)

#define SDW_ISHIM_PCMSYCM_LCHN			GENMASK(3, 0)
#define SDW_ISHIM_PCMSYCM_HCHN			GENMASK(7, 4)
#define SDW_ISHIM_PCMSYCM_STREAM		GENMASK(13, 8)
#define SDW_ISHIM_PCMSYCM_DIR			BIT(15)

#define SDW_ISHIM_PDMSCAP_ISS			GENMASK(3, 0)
#define SDW_ISHIM_PDMSCAP_OSS			GENMASK(7, 4)
#define SDW_ISHIM_PDMSCAP_BSS			GENMASK(12, 8)
#define SDW_ISHIM_PDMSCAP_CPSS			GENMASK(15, 13)

#define SDW_ISHIM_IOCTL_MIF			BIT(0)
#define SDW_ISHIM_IOCTL_CO			BIT(1)
#define SDW_ISHIM_IOCTL_COE			BIT(2)
#define SDW_ISHIM_IOCTL_DO			BIT(3)
#define SDW_ISHIM_IOCTL_DOE			BIT(4)
#define SDW_ISHIM_IOCTL_BKE			BIT(5)
#define SDW_ISHIM_IOCTL_WPDD			BIT(6)
#define SDW_ISHIM_IOCTL_CIBD			BIT(8)
#define SDW_ISHIM_IOCTL_DIBD			BIT(9)

#define SDW_ISHIM_CTMCTL_DACTQE			BIT(0)
#define SDW_ISHIM_CTMCTL_DODS			BIT(1)
#define SDW_ISHIM_CTMCTL_DOAIS			GENMASK(4, 3)

#define SDW_ISHIM_WAKEEN_ENABLE			BIT(0)
#define SDW_ISHIM_WAKESTS_STATUS		BIT(0)

/* Intel ALH Register definition */
#define SDW_IALH_STRMZCFG(x)			(0x000 + (0x4 * x))

#define SDW_IALH_STRMZCFG_DMAT_VAL		0x3
#define SDW_IALH_STRMZCFG_DMAT			GENMASK(7, 0)
#define SDW_IALH_STRMZCFG_CHN			GENMASK(19, 16)

/* single instance for SoundWire controller */
/* put callbacks for shim which can be called by master driver */

/**
 * struct sdw_ishim: Intel Shim context structure
 *
 * @shim: shim registers
 * @alh: Audio Link Hub (ALH) registers
 * @irq: interrupt number
 * @parent: parent device
 * @count: link count
 * @link: link instances
 * @config_ops: shim config ops
 */
struct sdw_ishim {
	void __iomem *shim;
	void __iomem *alh;
	int irq;
	struct device *parent;
	int count;
	struct sdw_ilink_data *link[SDW_MAX_LINKS];
	const struct sdw_config_ops *config_ops;
};


/*
 * read/write helpers
 */
static inline int sdw_ireg_readl(void __iomem *base, int offset)
{

	return readl(base + offset);
}

static inline void sdw_ireg_writel(void __iomem *base, int offset, int value)
{
	writel(value, base + offset);
}

static inline u16 sdw_ireg_readw(void __iomem *base, int offset)
{
	return readw(base + offset);
}

static inline void sdw_ireg_writew(void __iomem *base, int offset, u16 value)
{
	writew(value, base + offset);
}

static inline struct sdw_ilink_res *sdw_get_ilink(struct platform_device *pdev)
{
	return pdev->dev.platform_data;
}

/*
 * shim config ops
 */
static int sdw_ilink_power_down(struct sdw_ishim *shim, unsigned int link_id)
{

	void __iomem *shim_base = shim->link[link_id]->shim;
	volatile int link_control;
	int spa_mask, cpa_mask;
	int timeout = 10;
	u16 ioctl;

	/* Glue logic */
	ioctl = sdw_ireg_readw(shim_base, SDW_ISHIM_IOCTL(link_id));
	ioctl |= SDW_ISHIM_IOCTL_BKE;
	ioctl |= SDW_ISHIM_IOCTL_COE;
	sdw_ireg_writew(shim_base, SDW_ISHIM_IOCTL(link_id), ioctl);

	ioctl &= ~(SDW_ISHIM_IOCTL_MIF);
	sdw_ireg_writew(shim_base, SDW_ISHIM_IOCTL(link_id), ioctl);

	/* Link power down sequence */
	link_control = sdw_ireg_readl(shim_base, SDW_ISHIM_LCTL);
	spa_mask = ~(SDW_ISHIM_LCTL_SPA << link_id);
	cpa_mask = (SDW_ISHIM_LCTL_SPA << link_id);
	link_control &=  spa_mask;

	sdw_ireg_writel(shim_base, SDW_ISHIM_LCTL, link_control);
	do {
		link_control = sdw_ireg_readl(shim_base, SDW_ISHIM_LCTL);
		if (!(link_control & cpa_mask))
			break;
		timeout--;
		/* Wait for 20ms before each retry */
		msleep(20);
	} while (timeout != 0);

	/* Read once again to confirm */
	link_control = sdw_ireg_readl(shim_base, SDW_ISHIM_LCTL);
	if (!(link_control & cpa_mask))
		return 0;
	else
		return -EAGAIN;
}

static int sdw_ilink_power_up(struct sdw_ishim *shim, unsigned int link_id)
{

	void __iomem *shim_base = shim->link[link_id]->shim;
	volatile int link_control;
	int spa_mask, cpa_mask;
	int timeout = 10;

	/* Link power up sequence */
	link_control = sdw_ireg_readl(shim_base, SDW_ISHIM_LCTL);
	spa_mask = (SDW_ISHIM_LCTL_SPA << link_id);
	cpa_mask = (SDW_ISHIM_LCTL_SPA << link_id);
	link_control |=  spa_mask;

	sdw_ireg_writel(shim_base, SDW_ISHIM_LCTL, link_control);
	do {
		link_control = sdw_ireg_readl(shim_base, SDW_ISHIM_LCTL);
		if (link_control & cpa_mask)
			break;
		timeout--;
		/* Wait for 20ms before each retry */
		msleep(20);
	} while (timeout != 0);

	/* Read once again to confirm */
	link_control = sdw_ireg_readl(shim_base, SDW_ISHIM_LCTL);
	if (link_control & cpa_mask)
		return 0;
	else
		return -EAGAIN;

}

static void sdw_ishim_init(struct sdw_ishim *shim, unsigned int link_id)
{
	void __iomem *shim_base = shim->link[link_id]->shim;
	u16 ioctl = 0;
	u16 act = 0;

	/* Initialize Shim */
	ioctl |= SDW_ISHIM_IOCTL_BKE;
	sdw_ireg_writew(shim_base, SDW_ISHIM_IOCTL(link_id), ioctl);

	ioctl |= SDW_ISHIM_IOCTL_WPDD;
	sdw_ireg_writew(shim_base, SDW_ISHIM_IOCTL(link_id), ioctl);

	ioctl |= SDW_ISHIM_IOCTL_DO;
	sdw_ireg_writew(shim_base, SDW_ISHIM_IOCTL(link_id), ioctl);

	ioctl |= SDW_ISHIM_IOCTL_DOE;
	sdw_ireg_writew(shim_base, SDW_ISHIM_IOCTL(link_id), ioctl);

	/* Switch to MIP from Glue logic */
	ioctl = sdw_ireg_readw(shim_base,  SDW_ISHIM_IOCTL(link_id));

	ioctl &= ~(SDW_ISHIM_IOCTL_DOE);
	sdw_ireg_writew(shim_base, SDW_ISHIM_IOCTL(link_id), ioctl);

	ioctl &= ~(SDW_ISHIM_IOCTL_DO);
	sdw_ireg_writew(shim_base, SDW_ISHIM_IOCTL(link_id), ioctl);

	ioctl |= (SDW_ISHIM_IOCTL_MIF);
	sdw_ireg_writew(shim_base, SDW_ISHIM_IOCTL(link_id), ioctl);

	ioctl &= ~(SDW_ISHIM_IOCTL_BKE);
	ioctl &= ~(SDW_ISHIM_IOCTL_COE);

	sdw_ireg_writew(shim_base, SDW_ISHIM_IOCTL(link_id), ioctl);

	act |= 0x1 << SDW_REG_SHIFT(SDW_ISHIM_CTMCTL_DOAIS);
	act |= SDW_ISHIM_CTMCTL_DACTQE;
	act |= SDW_ISHIM_CTMCTL_DODS;
	sdw_ireg_writew(shim_base, SDW_ISHIM_CTMCTL(link_id), act);

}

static int sdw_ishim_sync(struct sdw_ishim *shim, unsigned int link_id,
					enum sdw_ishim_sync_ops ops)
{
	void __iomem *shim_base = shim->link[link_id]->shim;
	volatile int sync_up = 0;
	int timeout = 10;
	int sync_reg;

	/* Read SYNC register */
	sync_reg = sdw_ireg_readl(shim_base, SDW_ISHIM_SYNC);

	switch (ops) {
	case SDW_ISHIM_SYNCPRD:
		/* Set SyncPRD period */
		sync_reg |= (SDW_ISHIM_SYNC_SYNCPRD_VAL <<
				SDW_REG_SHIFT(SDW_ISHIM_SYNC_SYNCPRD));

		/* Set SyncCPU bit */
		sync_reg |= SDW_ISHIM_SYNC_SYNCCPU;
		sdw_ireg_writel(shim_base, SDW_ISHIM_SYNC, sync_reg);

		do {
			sync_up = sdw_ireg_readl(shim_base, SDW_ISHIM_SYNC);
			if ((sync_up & SDW_ISHIM_SYNC_SYNCCPU) == 0)
				break;
			timeout--;

			/* Wait 20ms before each try */
			msleep(20);

		} while (timeout != 0);

		if ((sync_up & SDW_ISHIM_SYNC_SYNCCPU) != 0)
			return -EIO;
		break;

	case SDW_ISHIM_SYNCGO:
		/* Check CMDSYNC bit set for any Master */
		if (!(sync_reg & SDW_ISHIM_SYNC_CMDSYNC_MASK))
			return 0;

		/* Set SyncGO bit */
		sync_reg |= SDW_ISHIM_SYNC_SYNCGO;
		sdw_ireg_writel(shim_base, SDW_ISHIM_SYNC, sync_reg);

		do {
			sync_up = sdw_ireg_readl(shim_base, SDW_ISHIM_SYNC);
			if ((sync_up & SDW_ISHIM_SYNC_SYNCGO) == 0)
				break;
			timeout--;

			/* Wait 20ms before each try */
			msleep(20);

		} while (timeout != 0);

		if ((sync_up & SDW_ISHIM_SYNC_SYNCGO) != 0)
			return -EIO;
		break;

	case SDW_ISHIM_CMDSYNC:
		sync_reg |= SDW_ISHIM_SYNC_CMDSYNC << link_id;
		sdw_ireg_writel(shim_base, SDW_ISHIM_SYNC, sync_reg);
		break;

	default:
		return -EINVAL;
		break;
	}

	return 0;
}

static void sdw_ishim_pdi_init(struct sdw_ishim *shim, unsigned int link_id,
					struct sdw_cdns_stream_config *config)
{
	void __iomem *shim_base = shim->link[link_id]->shim;
	int pcm_cap;
	int pdm_cap;

	/* PCM Stream Capability */
	pcm_cap = sdw_ireg_readw(shim_base, SDW_ISHIM_PCMSCAP(link_id));

	config->pcm_bd = (pcm_cap & SDW_ISHIM_PCMSCAP_BSS) >>
					SDW_REG_SHIFT(SDW_ISHIM_PCMSCAP_BSS);
	config->pcm_in = (pcm_cap & SDW_ISHIM_PCMSCAP_ISS) >>
					SDW_REG_SHIFT(SDW_ISHIM_PCMSCAP_ISS);
	config->pcm_out = (pcm_cap & SDW_ISHIM_PCMSCAP_OSS) >>
					SDW_REG_SHIFT(SDW_ISHIM_PCMSCAP_OSS);


	/* PDM Stream Capability */
	pdm_cap = sdw_ireg_readw(shim_base, SDW_ISHIM_PDMSCAP(link_id));

	config->pdm_bd = (pdm_cap & SDW_ISHIM_PDMSCAP_BSS) >>
					SDW_REG_SHIFT(SDW_ISHIM_PDMSCAP_BSS);
	config->pdm_in = (pdm_cap & SDW_ISHIM_PDMSCAP_ISS) >>
					SDW_REG_SHIFT(SDW_ISHIM_PDMSCAP_ISS);
	config->pdm_out = (pdm_cap & SDW_ISHIM_PDMSCAP_OSS) >>
					SDW_REG_SHIFT(SDW_ISHIM_PDMSCAP_OSS);

}

static int sdw_ishim_pdi_ch_cap(struct sdw_ishim *shim, unsigned int link_id,
					unsigned int pdi_num, bool pcm)
{
	void __iomem *shim_base = shim->link[link_id]->shim;
	int count;

	if (pcm) {
		count = sdw_ireg_readw(shim_base,
					SDW_ISHIM_PCMSYCHC(link_id, pdi_num));

	} else {
		count = sdw_ireg_readw(shim_base, SDW_ISHIM_PDMSCAP(link_id));
		count = ((count & SDW_ISHIM_PDMSCAP_CPSS) >>
					SDW_REG_SHIFT(SDW_ISHIM_PDMSCAP_CPSS));
	}

	/* zero based values for channel count in register */
	count++;

	return count;
}

static int sdw_ishim_pdi_conf(struct sdw_ishim *shim, unsigned int link_id,
						struct sdw_cdns_pdi *info,
						enum sdw_ireg_type reg_type)
{
	void __iomem *shim_base = shim->link[link_id]->shim;
	void __iomem *alh_base = shim->link[link_id]->alh;
	unsigned int strm_conf = 0;
	int pdi_conf = 0;

	switch (reg_type) {

	case SDW_REG_ISHIM:

		/*
		 * Program stream parameters to stream SHIM register
		 * This is applicable for PCM stream only.
		 */
		if (!(info->type == SDW_STREAM_PCM))
			return 0;

		if (info->dir == SDW_DATA_DIR_IN)
			pdi_conf |= SDW_ISHIM_PCMSYCM_DIR;
		else
			pdi_conf &= ~(SDW_ISHIM_PCMSYCM_DIR);

		pdi_conf |= (info->stream_num << SDW_REG_SHIFT(SDW_ISHIM_PCMSYCM_STREAM));
		pdi_conf |= (info->l_ch_num << SDW_REG_SHIFT(SDW_ISHIM_PCMSYCM_LCHN));
		pdi_conf |= (info->h_ch_num << SDW_REG_SHIFT(SDW_ISHIM_PCMSYCM_HCHN));

		sdw_ireg_writew(shim_base,
				SDW_ISHIM_PCMSYCHM(link_id, info->pdi_num),
				pdi_conf);
		break;

	case SDW_REG_IALH:

		/* Program Stream config ALH register */
		strm_conf = sdw_ireg_readl(alh_base,
					SDW_IALH_STRMZCFG(info->stream_num));

		strm_conf |= (SDW_IALH_STRMZCFG_DMAT_VAL <<
					SDW_REG_SHIFT(SDW_IALH_STRMZCFG_DMAT));

		strm_conf |= ((info->ch_count - 1) <<
					SDW_REG_SHIFT(SDW_IALH_STRMZCFG_CHN));

		sdw_ireg_writel(alh_base, SDW_IALH_STRMZCFG(info->stream_num),
						strm_conf);
		break;

	default:
		return -EINVAL;
		break;

	}

	return 0;
}

static void sdw_ilink_shim_wake(struct sdw_ishim *shim, unsigned int link_id,
							bool wake_enable)
{
	void __iomem *shim_base = shim->link[link_id]->shim;
	u16 wake_en, wake_sts;

	if (wake_enable) {
		/* Enable the wakeup */
		sdw_ireg_writew(shim_base, SDW_ISHIM_WAKEEN,
					(SDW_ISHIM_WAKEEN_ENABLE << link_id));
	} else {
		/* Disable the wake up interrupt */
		wake_en = sdw_ireg_readw(shim_base, SDW_ISHIM_WAKEEN);
		wake_en &= ~(SDW_ISHIM_WAKEEN_ENABLE << link_id);
		sdw_ireg_writew(shim_base, SDW_ISHIM_WAKEEN, wake_en);

		/* Clear wake status */
		wake_sts = sdw_ireg_readw(shim_base, SDW_ISHIM_WAKESTS);
		wake_sts |= (SDW_ISHIM_WAKEEN_ENABLE << link_id);
		sdw_ireg_writew(shim_base, SDW_ISHIM_WAKESTS_STATUS, wake_sts);
	}
}

static int sdw_ilink_config_pdi(struct sdw_ishim *shim, unsigned int link_id,
		struct sdw_cdns_pdi *pdi)
{
	void __iomem *shim_base = shim->link[link_id]->shim;
	void __iomem *alh_base = shim->link[link_id]->alh;
	u32 val = 0, str_id;

	if (pdi->dir == SDW_DATA_DIR_IN)
		val = SDW_ISHIM_PCMSYCM_DIR;

	/* TODO decode the magic here */
	str_id = link_id + 1 + pdi->pdi_num + 5;

	val |= str_id << fls(SDW_ISHIM_PCMSYCM_STREAM);
	val |= pdi->l_ch_num << fls(SDW_ISHIM_PCMSYCM_LCHN);
	val |= pdi->h_ch_num << fls(SDW_ISHIM_PCMSYCM_HCHN);
	sdw_ireg_writew(shim_base, SDW_ISHIM_PCMSYCHM(link_id, pdi->pdi_num), val);

	val = sdw_ireg_readw(alh_base, SDW_IALH_STRMZCFG(pdi->pdi_num));
	val |= SDW_IALH_STRMZCFG_DMAT_VAL | pdi->h_ch_num << fls(SDW_IALH_STRMZCFG_CHN);
	sdw_ireg_writel(alh_base, SDW_IALH_STRMZCFG(pdi->pdi_num), val);

	return 0;
}

static int sdw_ilink_config_stream(struct sdw_ishim *shim, unsigned int link_id,
				void *substream, void *dai, void *hw_params)
{
	return shim->config_ops->config_stream(substream, dai, hw_params);
}

static const struct sdw_ishim_ops ishim_ops = {
	.link_power_down = sdw_ilink_power_down,
	.link_power_up = sdw_ilink_power_up,
	.init = sdw_ishim_init,
	.sync = sdw_ishim_sync,
	.pdi_init = sdw_ishim_pdi_init,
	.pdi_ch_cap = sdw_ishim_pdi_ch_cap,
	.pdi_conf = sdw_ishim_pdi_conf,
	.wake = sdw_ilink_shim_wake,
	.config_pdi = sdw_ilink_config_pdi,
	.config_stream = sdw_ilink_config_stream,
};

/*
 * shim init routines
 */
static int intel_sdw_cleanup_pdev(struct sdw_ishim *shim)
{
	int i;

	for (i = 0; i < shim->count; i++) {
		if (shim->link[i]->pdev)
			platform_device_unregister(shim->link[i]->pdev);
	}
	return 0;
}

static struct sdw_ishim *intel_sdw_add_controller(struct intel_sdw_res *res)
{
	struct acpi_device *adev;
	struct platform_device *pdev;
	struct sdw_ishim *shim;
	struct sdw_ilink_res *link_res;
	struct platform_device_info pdevinfo;
	u8 count;
	u32 caps;
	int ret, i;

	if (acpi_bus_get_device(res->parent, &adev))
		return NULL;

	/* now we found the controller, so find the links supported */
	count = 0;
	ret = fwnode_property_read_u8_array(acpi_fwnode_handle(adev),
				  "mipi-sdw-master-count", &count, 1);
	if (ret) {
		dev_err(&adev->dev, "Failed to read mipi-sdw-master-count: %d\n", ret);
		return NULL;
	}

	shim = kzalloc(sizeof(*shim), GFP_KERNEL);
	if (!shim)
		return NULL;

	shim->shim = res->mmio_base + SDW_ISHIM_BASE;
	shim->alh = res->mmio_base + SDW_IALH_BASE;
	shim->irq = res->irq;
	shim->parent = res->parent;
	shim->config_ops = res->config_ops;

	/* Check the SNDWLCAP.LCOUNT */
	caps = ioread32(shim->shim + SDW_ISHIM_LCAP);

	/* check HW supports vs property value and use min of two */
	count = min_t(u8, caps, count);

	dev_info(&adev->dev, "Creating %d SDW Link devices\n", count);
	shim->count = count;

	/* create those devices */
	for (i = 0; i < count; i++) {

		/* SRK: This should be inside for loop for each master instance */
		link_res = kmalloc(sizeof(*link_res), GFP_KERNEL);
		if (!link_res)
			goto link_err;

		link_res->irq = res->irq;
		link_res->registers = res->mmio_base + SDW_ILINK_BASE
					+ (SDW_ILINK_SIZE * i);
		link_res->shim = shim;
		link_res->ops = &ishim_ops;

		memset(&pdevinfo, 0, sizeof(pdevinfo));

		pdevinfo.parent = res->parent;
		pdevinfo.name = "int-sdw";
		pdevinfo.id = i;
		pdevinfo.fwnode = acpi_fwnode_handle(adev);
		pdevinfo.data = link_res;
		pdevinfo.size_data = sizeof(*link_res);

		pdev = platform_device_register_full(&pdevinfo);
		if (IS_ERR(pdev)) {
			dev_err(&adev->dev, "platform device creation failed: %ld\n",
				PTR_ERR(pdev));
			goto pdev_err;
		} else {
			dev_dbg(&adev->dev, "created platform device %s\n",
				dev_name(&pdev->dev));
		}

		shim->link[i]->pdev = pdev;
		shim->link[i]->shim = link_res->registers + SDW_ISHIM_BASE;
		shim->link[i]->alh = link_res->registers + SDW_ILINK_BASE;

		kfree(link_res);
	}

	return shim;

pdev_err:
	intel_sdw_cleanup_pdev(shim);
link_err:
	kfree(shim);
	return NULL;
}

static acpi_status intel_sdw_acpi_cb(acpi_handle handle, u32 level,
					void *cdata, void **return_value)
{
	struct acpi_device *adev;

	if (acpi_bus_get_device(handle, &adev))
		return AE_NOT_FOUND;

	dev_dbg(&adev->dev, "Found ACPI handle\n");

	return AE_OK;
}

void *intel_sdw_init(acpi_handle *parent_handle, struct intel_sdw_res *res)
{
	acpi_status status;

	status = acpi_walk_namespace(ACPI_TYPE_DEVICE, parent_handle, 1,
				     intel_sdw_acpi_cb, NULL,
				     NULL, NULL);
	if (ACPI_FAILURE(status)) {
		pr_err("Intel SDW: failed to find controller: %d\n", status);
		return NULL;
	}

	return intel_sdw_add_controller(res);
}
EXPORT_SYMBOL_GPL(intel_sdw_init);

void intel_sdw_exit(void *arg)
{
	struct sdw_ishim *shim = arg;

	intel_sdw_cleanup_pdev(shim);
	kfree(shim);
}
EXPORT_SYMBOL_GPL(intel_sdw_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Intel Soundwire Shim driver");
