/*
 * Renesas USB3.0 Peripheral driver (USB gadget)
 *
 * Copyright (C) 2014-2015  Renesas Electronics Corporation
 *
 * Author : Yoshihiro Shimoda <yoshihiro.shimoda.uh@renesas.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 */

#ifndef __RENESAS_USB3_H__
#define __RENESAS_USB3_H__

/* register definitions */
#define USB3_AXI_INT_STA	0x008
#define USB3_AXI_INT_ENA	0x00c
#define USB3_DMA_INT_STA	0x010
#define USB3_DMA_INT_ENA	0x014
#define USB3_USB_COM_CON	0x200
#define USB3_USB20_CON		0x204
#define USB3_USB30_CON		0x208
#define USB3_USB_STA		0x210
#define USB3_DRD_CON		0x218
#define USB3_USB_INT_STA_1	0x220
#define USB3_USB_INT_STA_2	0x224
#define USB3_USB_INT_ENA_1	0x228
#define USB3_USB_INT_ENA_2	0x22c
#define USB3_STUP_DAT_0		0x230
#define USB3_STUP_DAT_1		0x234
#define USB3_P0_MOD		0x280
#define USB3_P0_CON		0x288
#define USB3_P0_STA		0x28c
#define USB3_P0_INT_STA		0x290
#define USB3_P0_INT_ENA		0x294
#define USB3_P0_LNG		0x2a0
#define USB3_P0_READ		0x2a4
#define USB3_P0_WRITE		0x2a8
#define USB3_PIPE_COM		0x2b0
#define USB3_PN_MOD		0x2c0
#define USB3_PN_RAMMAP		0x2c4
#define USB3_PN_CON		0x2c8
#define USB3_PN_STA		0x2cc
#define USB3_PN_INT_STA		0x2d0
#define USB3_PN_INT_ENA		0x2d4
#define USB3_PN_LNG		0x2e0
#define USB3_PN_READ		0x2e4
#define USB3_PN_WRITE		0x2e8
#define USB3_SSIFCMD		0x340

/* AXI_INT_ENA and AXI_INT_STA */
#define AXI_INT_DMAINT		BIT(31)
#define AXI_INT_EPCINT		BIT(30)

/* LCLKSEL */
#define LCLKSEL_LSEL		BIT(18)

/* USB_COM_CON */
#define USB_COM_CON_CONF		BIT(24)
#define USB_COM_CON_SPD_MODE		BIT(17)
#define USB_COM_CON_EP0_EN		BIT(16)
#define USB_COM_CON_DEV_ADDR_SHIFT	8
#define USB_COM_CON_DEV_ADDR_MASK	GENMASK(14, USB_COM_CON_DEV_ADDR_SHIFT)
#define USB_COM_CON_DEV_ADDR(n)		(((n) << USB_COM_CON_DEV_ADDR_SHIFT) & \
					 USB_COM_CON_DEV_ADDR_MASK)
#define USB_COM_CON_RX_DETECTION	BIT(1)
#define USB_COM_CON_PIPE_CLR		BIT(0)

/* USB20_CON */
#define USB20_CON_B2_PUE		BIT(31)
#define USB20_CON_B2_SUSPEND		BIT(24)
#define USB20_CON_B2_CONNECT		BIT(17)
#define USB20_CON_B2_TSTMOD_SHIFT	8
#define USB20_CON_B2_TSTMOD_MASK	GENMASK(10, USB20_CON_B2_TSTMOD_SHIFT)
#define USB20_CON_B2_TSTMOD(n)		(((n) << USB20_CON_B2_TSTMOD_SHIFT) & \
					 USB20_CON_B2_TSTMOD_MASK)
#define USB20_CON_B2_TSTMOD_EN		BIT(0)

/* USB30_CON */
#define USB30_CON_POW_SEL_SHIFT		24
#define USB30_CON_POW_SEL_MASK		GENMASK(26, USB30_CON_POW_SEL_SHIFT)
#define USB30_CON_POW_SEL_IN_U3		BIT(26)
#define USB30_CON_POW_SEL_IN_DISCON	0
#define USB30_CON_POW_SEL_P2_TO_P0	BIT(25)
#define USB30_CON_POW_SEL_P0_TO_P3	BIT(24)
#define USB30_CON_POW_SEL_P0_TO_P2	0
#define USB30_CON_B3_PLLWAKE		BIT(23)
#define USB30_CON_B3_CONNECT		BIT(17)
#define USB30_CON_B3_HOTRST_CMP		BIT(1)

/* USB_STA */
#define USB_STA_SPEED_MASK	(BIT(2) | BIT(1))
#define USB_STA_SPEED_HS	BIT(2)
#define USB_STA_SPEED_FS	BIT(1)
#define USB_STA_SPEED_SS	0
#define USB_STA_VBUS_STA	BIT(0)

/* DRD_CON */
#define DRD_CON_PERI_CON	BIT(24)

/* USB_INT_ENA_1 and USB_INT_STA_1 */
#define USB_INT_1_B3_PLLWKUP	BIT(31)
#define USB_INT_1_B3_LUPSUCS	BIT(30)
#define USB_INT_1_B3_DISABLE	BIT(27)
#define USB_INT_1_B3_WRMRST	BIT(21)
#define USB_INT_1_B3_HOTRST	BIT(20)
#define USB_INT_1_B2_USBRST	BIT(12)
#define USB_INT_1_B2_L1SPND	BIT(11)
#define USB_INT_1_B2_SPND	BIT(9)
#define USB_INT_1_B2_RSUM	BIT(8)
#define USB_INT_1_SPEED		BIT(1)
#define USB_INT_1_VBUS_CNG	BIT(0)

/* USB_INT_ENA_2 and USB_INT_STA_2 */
#define USB_INT_2_PIPE(n)	BIT(n)

/* P0_MOD */
#define P0_MOD_DIR		BIT(6)

/* P0_CON and PN_CON */
#define PX_CON_BYTE_EN_MASK		(BIT(10) | BIT(9))
#define PX_CON_BYTE_EN_SHIFT		9
#define PX_CON_BYTE_EN_BYTES(n)		(((n) << PX_CON_BYTE_EN_SHIFT) & \
					 PX_CON_BYTE_EN_MASK)
#define PX_CON_SEND			BIT(8)

/* P0_CON */
#define P0_CON_ST_RES_MASK		(BIT(27) | BIT(26))
#define P0_CON_ST_RES_FORCE_STALL	BIT(27)
#define P0_CON_ST_RES_NORMAL		BIT(26)
#define P0_CON_ST_RES_FORCE_NRDY	0
#define P0_CON_OT_RES_MASK		(BIT(25) | BIT(24))
#define P0_CON_OT_RES_FORCE_STALL	BIT(25)
#define P0_CON_OT_RES_NORMAL		BIT(24)
#define P0_CON_OT_RES_FORCE_NRDY	0
#define P0_CON_IN_RES_MASK		(BIT(17) | BIT(16))
#define P0_CON_IN_RES_FORCE_STALL	BIT(17)
#define P0_CON_IN_RES_NORMAL		BIT(16)
#define P0_CON_IN_RES_FORCE_NRDY	0
#define P0_CON_RES_WEN			BIT(7)
#define P0_CON_BCLR			BIT(1)

/* P0_STA and PN_STA */
#define PX_STA_BUFSTS		BIT(0)

/* P0_INT_ENA and P0_INT_STA */
#define P0_INT_STSED		BIT(18)
#define P0_INT_STSST		BIT(17)
#define P0_INT_SETUP		BIT(16)
#define P0_INT_RCVNL		BIT(8)
#define P0_INT_ERDY		BIT(7)
#define P0_INT_FLOW		BIT(6)
#define P0_INT_STALL		BIT(2)
#define P0_INT_NRDY		BIT(1)
#define P0_INT_BFRDY		BIT(0)
#define P0_INT_ALL_BITS		(P0_INT_STSED | P0_INT_SETUP | P0_INT_BFRDY)


/* PN_MOD */
#define PN_MOD_DIR		BIT(6)
#define PN_MOD_TYPE_SHIFT	4
#define PN_MOD_TYPE_MASK	GENMASK(5, PN_MOD_TYPE_SHIFT)
#define PN_MOD_TYPE(n)		(((n) << PN_MOD_TYPE_SHIFT) & \
				 PN_MOD_TYPE_MASK)
#define PN_MOD_EPNUM_MASK	GENMASK(3, 0)
#define PN_MOD_EPNUM(n)		((n) & PN_MOD_EPNUM_MASK)

/* PN_RAMMAP */
#define PN_RAMMAP_RAMAREA_SHIFT	29
#define PN_RAMMAP_RAMAREA_MASK	GENMASK(31, PN_RAMMAP_RAMAREA_SHIFT)
#define PN_RAMMAP_RAMAREA_16KB	BIT(31)
#define PN_RAMMAP_RAMAREA_8KB	(BIT(30) | BIT(29))
#define PN_RAMMAP_RAMAREA_4KB	BIT(30)
#define PN_RAMMAP_RAMAREA_2KB	BIT(29)
#define PN_RAMMAP_RAMAREA_1KB	0
#define PN_RAMMAP_MPKT_SHIFT	16
#define PN_RAMMAP_MPKT_MASK	GENMASK(26, PN_RAMMAP_MPKT_SHIFT)
#define PN_RAMMAP_MPKT(n)	(((n) << PN_RAMMAP_MPKT_SHIFT) & \
				 PN_RAMMAP_MPKT_MASK)
#define PN_RAMMAP_RAMIF_SHIFT	14
#define PN_RAMMAP_RAMIF_MASK	GENMASK(15, PN_RAMMAP_RAMIF_SHIFT)
#define PN_RAMMAP_RAMIF(n)	(((n) << PN_RAMMAP_RAMIF_SHIFT) & \
				 PN_RAMMAP_RAMIF_MASK)
#define PN_RAMMAP_BASEAD_MASK	GENMASK(13, 0)
#define PN_RAMMAP_BASEAD(offs)	(((offs) >> 3) & PN_RAMMAP_BASEAD_MASK)
#define PN_RAMMAP_DATA(area, ramif, basead)	((PN_RAMMAP_##area) | \
						 (PN_RAMMAP_RAMIF(ramif)) | \
						 (PN_RAMMAP_BASEAD(basead)))

/* PN_CON */
#define PN_CON_EN		BIT(31)
#define PN_CON_DATAIF_EN	BIT(30)
#define PN_CON_RES_MASK		(BIT(17) | BIT(16))
#define PN_CON_RES_FORCE_STALL	BIT(17)
#define PN_CON_RES_NORMAL	BIT(16)
#define PN_CON_RES_FORCE_NRDY	0
#define PN_CON_LAST		BIT(11)
#define PN_CON_RES_WEN		BIT(7)
#define PN_CON_CLR		BIT(0)

/* PN_INT_STA and PN_INT_ENA */
#define PN_INT_LSTTR	BIT(4)
#define PN_INT_BFRDY	BIT(0)

/* USB3_SSIFCMD */
#define SSIFCMD_URES_U2		BIT(9)
#define SSIFCMD_URES_U1		BIT(8)
#define SSIFCMD_UDIR_U2		BIT(7)
#define SSIFCMD_UDIR_U1		BIT(6)
#define SSIFCMD_UREQ_U2		BIT(5)
#define SSIFCMD_UREQ_U1		BIT(4)

#define USB3_EP0_SS_MAX_PACKET_SIZE	512
#define USB3_EP0_HSFS_MAX_PACKET_SIZE	64
#define USB3_EP0_BUF_SIZE		8
#define USB3_MAX_NUM_PIPES		30
#define USB3_WAIT_NS			3000

struct renesas_usb3;
struct renesas_usb3_request {
	struct usb_request	req;
	struct list_head	queue;
};

#define USB3_EP_NAME_SIZE	8
struct renesas_usb3_ep {
	struct usb_ep ep;
	struct renesas_usb3 *usb3;
	int num;
	char ep_name[USB3_EP_NAME_SIZE];
	struct list_head queue;
	u32 rammap_val;
	bool dir_in;
	bool halt;
	bool wedge;
	bool started;
};

struct renesas_usb3_priv {
	int ramsize_per_ramif;		/* unit = bytes */
	int num_ramif;
	int ramsize_per_pipe;		/* unit = bytes */
	unsigned workaround_for_vbus:1;	/* if 1, don't check vbus signal */
};

struct renesas_usb3 {
	void __iomem *reg;

	struct usb_gadget gadget;
	struct usb_gadget_driver *driver;

	struct renesas_usb3_ep *usb3_ep;
	int num_usb3_eps;

	spinlock_t lock;
	int disabled_count;

	struct usb_request *ep0_req;
	u16 test_mode;
	u8 ep0_buf[USB3_EP0_BUF_SIZE];
	unsigned softconnect:1;
	unsigned workaround_for_vbus:1;
};

#define gadget_to_renesas_usb3(_gadget)	\
		container_of(_gadget, struct renesas_usb3, gadget)
#define renesas_usb3_to_gadget(renesas_usb3) (&renesas_usb3->gadget)
#define usb3_to_dev(_usb3)	(_usb3->gadget.dev.parent)

#define usb_ep_to_usb3_ep(_ep) container_of(_ep, struct renesas_usb3_ep, ep)
#define usb3_ep_to_usb3(_usb3_ep) (_usb3_ep->usb3)
#define usb_req_to_usb3_req(_req) container_of(_req, \
					    struct renesas_usb3_request, req)


#define usb3_get_ep(usb3, n) ((usb3)->usb3_ep + (n))
#define usb3_for_each_ep(usb3_ep, usb3, i)			\
		for ((i) = 0, usb3_ep = usb3_get_ep(usb3, (i));	\
		     (i) < (usb3)->num_usb3_eps;		\
		     (i)++, usb3_ep = usb3_get_ep(usb3, (i)))

#endif	/* __RENESAS_USB3_H__ */

