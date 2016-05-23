/*
 * u_uac1.h -- interface to USB gadget "ALSA AUDIO" utilities
 *
 * Copyright (C) 2008 Bryan Wu <cooloney@kernel.org>
 * Copyright (C) 2008 Analog Devices, Inc
 *
 * Enter bugs at http://blackfin.uclinux.org/
 *
 * Licensed under the GPL-2 or later.
 */

#ifndef __U_UAC1_H
#define __U_UAC1_H

#include <linux/device.h>
#include <linux/err.h>
#include <linux/usb/audio.h>
#include <linux/usb/composite.h>

#define UAC1_OUT_EP_MAX_PACKET_SIZE	200
#define UAC1_DEF_CCHMASK	0x3
#define UAC1_DEF_CSRATE		48000
#define UAC1_DEF_CSSIZE		2


struct f_uac1_opts {
	struct usb_function_instance	func_inst;
	int				c_chmask;
	int				c_srate;
	int				c_ssize;
	unsigned			bound:1;

	struct mutex			lock;
	int				refcnt;
};

#endif /* __U_UAC1_H */
