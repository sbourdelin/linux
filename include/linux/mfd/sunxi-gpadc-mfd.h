#ifndef __SUNXI_GPADC_MFD__H__
#define __SUNXI_GPADC_MFD__H__

#define TP_INT_FIFOC            0x10
#define TP_INT_FIFOS            0x14

struct sunxi_gpadc_mfd_dev {
	void __iomem			*regs;
	struct device			*dev;
	struct regmap			*regmap;
	struct regmap_irq_chip_data	*regmap_irqc;
};

#endif
