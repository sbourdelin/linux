/*
 * Eberspächer Flexcard PMC II Carrier Board PCI Driver - device attributes
 *
 * Copyright (c) 2014 - 2016, Linutronix GmbH
 * Author: Benedikt Spranger <b.spranger@linutronix.de>
 *         Holger Dengler <dengler@linutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#ifndef _LINUX_MFD_FLEXCARD_H
#define _LINUX_MFD_FLEXCARD_H

#include <uapi/linux/flexcard.h>

/* PCI BAR 0: Flexcard DMA register */
struct fc_bar0_dma {
	__u32 dma_ctrl;			/* 500 */
	__u32 dma_stat;			/* 504 */
	__u32 r16[2];			/* 508 */
	__u64 dma_cba;			/* 510 */
	__u32 dma_cbs;			/* 518 */
	__u32 dma_txr;			/* 51c */
	__u32 dma_irer;			/* 520 */
	__u32 dma_irsr;			/* 524 */
	__u32 r17[10];			/* 528 */
	__u32 dma_cbcr;			/* 550 */
	__u32 dma_cblr;			/* 554 */
	__u32 r18[2];			/* 558 */
	__u32 dma_itcr;			/* 560 */
	__u32 dma_itr;			/* 564 */
	__u32 r19[2];			/* 568 */
	__u32 dma_wptr;			/* 570 */
	__u32 dma_rptr;			/* 574 */
} __packed;

/* PCI BAR 0: Flexcard clock register */
struct fc_bar0_time {
	__u32 ts_high;			/* 700 */
	__u32 ts_low;			/* 704 */
	__u32 r21[2];			/* 708 */
	__u32 clk_src;			/* 710 */
} __packed;

struct fc_bar0_nf {
	__u32 fc_nfctrl;		/* 170 */
	__u32 nf_cnt;			/* 174 */
} __packed;

/* PCI BAR 0: Flexcard register */
struct fc_bar0 {
	struct fc_bar0_conf conf;	/* 000-13c */
	__u32 fc_ts;			/* 140 */
	__u32 fc_reset;			/* 144 */
	__u32 trig_sc_ctrl;		/* 148 */
	__u32 trig_ctrl;		/* 14c */
	__u32 r12;			/* 150 */
	__u32 tirqir;			/* 154 */
	__u32 pccr1;			/* 158 */
	__u32 pccr2;			/* 15c */
	__u32 r13[4];			/* 160 */
	struct fc_bar0_nf nf;		/* 170-174 */
	__u32 r14;			/* 178 */
	struct fc_bar0_dma dma;		/* 500-574 */
	__u32 r20[0x62];		/* 578 */
	struct fc_bar0_time time;	/* 700-710 */
	__u32 r22[0x7b];		/* 714 */
	__u32 faddr;			/* 900 */
	__u32 fwdat;			/* 904 */
	__u32 fctrl;			/* 908 */
	__u32 frdat;			/* 90c */
	__u32 bwdat[16];		/* 910 */
	__u32 brdat[16];		/* 950 */
	__u32 r23[28];			/* 990 */
	__u32 fwmode;			/* a00 */
	__u32 recond;			/* a04 */
	__u32 wdtctrl;			/* a08 */
	__u32 imgsel;			/* a0c */
	__u32 actimg;			/* a10 */
	__u32 updimginf;		/* a14 */
	__u32 r24[0x32];		/* a18 */
	__u32 factory_image_info[8];	/* ae0 */
	__u32 app_image0_info[8];	/* b00 */
	__u32 app_image1_info[8];	/* b20 */
	__u32 app_image2_info[8];	/* b40 */
	__u32 app_image3_info[8];	/* b60 */
	__u32 app_image4_info[8];	/* b80 */
	__u32 app_image5_info[8];	/* ba0 */
	__u32 app_image6_info[8];	/* bc0 */
	__u32 app_image7_info[8];	/* be0 */
	__u32 r25[0x100];		/* c00 */
} __packed;

struct flexcard_device {
	unsigned int cardnr;
	struct pci_dev *pdev;
	raw_spinlock_t irq_lock;
	struct irq_domain *irq_domain;
	struct irq_domain *dma_domain;
	struct fc_bar0 __iomem *bar0;
	struct mfd_cell *cells;
	struct resource *res;
	u32 dev_irqmsk;
	u32 dma_irqmsk;
};

int flexcard_setup_irq(struct pci_dev *pdev);
void flexcard_remove_irq(struct pci_dev *pdev);

int flexcard_register_rx_cb(int cc, void *priv,
			    int (*rx_cb)(void *priv, void *data, size_t len));
void flexcard_unregister_rx_cb(int cc);

#endif /* _LINUX_FLEXCARD_H */
