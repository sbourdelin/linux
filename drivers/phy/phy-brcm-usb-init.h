/*
 * Copyright (C) 2014-2017 Broadcom
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _USB_BRCM_COMMON_INIT_H
#define _USB_BRCM_COMMON_INIT_H

#define USB_CTLR_DEVICE_OFF 0
#define USB_CTLR_DEVICE_ON 1
#define USB_CTLR_DEVICE_DUAL 2
#define USB_CTLR_DEVICE_TYPEC_PD 3

struct  brcm_usb_init_params;

struct brcm_usb_init_ops {
	void (*init_ipp)(struct brcm_usb_init_params *params);
	void (*init_common)(struct brcm_usb_init_params *params);
	void (*init_eohci)(struct brcm_usb_init_params *params);
	void (*init_xhci)(struct brcm_usb_init_params *params);
	void (*uninit_common)(struct brcm_usb_init_params *params);
	void (*uninit_eohci)(struct brcm_usb_init_params *params);
	void (*uninit_xhci)(struct brcm_usb_init_params *params);
};

struct  brcm_usb_init_params {
	void __iomem *ctrl_regs;
	void __iomem *xhci_ec_regs;
	int ioc;
	int ipp;
	int device_mode;
	u32 family_id;
	u32 product_id;
	int selected_family;
	const char *family_name;
	const u32 *usb_reg_bits_map;
	const struct brcm_usb_init_ops *ops;
};

void brcm_usb_set_family_map(struct brcm_usb_init_params *params);
int brcm_usb_init_get_dual_select(struct brcm_usb_init_params *params);
void brcm_usb_init_set_dual_select(struct brcm_usb_init_params *params,
				int mode);

static inline void brcm_usb_init_ipp(struct brcm_usb_init_params *ini)
{
	if (ini->ops->init_ipp)
		ini->ops->init_ipp(ini);
}

static inline void brcm_usb_init_common(struct brcm_usb_init_params *ini)
{
	if (ini->ops->init_common)
		ini->ops->init_common(ini);
}

static inline void brcm_usb_init_eohci(struct brcm_usb_init_params *ini)
{
	if (ini->ops->init_eohci)
		ini->ops->init_eohci(ini);
}

static inline void brcm_usb_init_xhci(struct brcm_usb_init_params *ini)
{
	if (ini->ops->init_xhci)
		ini->ops->init_xhci(ini);
}

static inline void brcm_usb_uninit_common(struct brcm_usb_init_params *ini)
{
	if (ini->ops->uninit_common)
		ini->ops->uninit_common(ini);
}

static inline void brcm_usb_uninit_eohci(struct brcm_usb_init_params *ini)
{
	if (ini->ops->uninit_eohci)
		ini->ops->uninit_eohci(ini);
}

static inline void brcm_usb_uninit_xhci(struct brcm_usb_init_params *ini)
{
	if (ini->ops->uninit_xhci)
		ini->ops->uninit_xhci(ini);
}

#endif /* _USB_BRCM_COMMON_INIT_H */
