// SPDX-License-Identifier: GPL-2.0+
/*
 * u_uac3.h
 *
 * Utility definitions for UAC3 function
 *
 * Author: Ruslan Bilovol <ruslan.bilovol@gmail.com>
 */

#ifndef __U_UAC3_H
#define __U_UAC3_H

#include <linux/usb/composite.h>

#define UAC3_DEF_PCHMASK 0x3
#define UAC3_DEF_PSRATE 48000
#define UAC3_DEF_PSSIZE 2
#define UAC3_DEF_CCHMASK 0x3
#define UAC3_DEF_CSRATE 48000
#define UAC3_DEF_CSSIZE 2
#define UAC3_DEF_REQ_NUM 2

struct f_uac3_opts {
	struct usb_function_instance	func_inst;
	int				p_chmask;
	int				p_srate;
	int				p_ssize;
	int				c_chmask;
	int				c_srate;
	int				c_ssize;
	int				req_number;
	bool				bound;

	struct mutex			lock;
	int				refcnt;
};

#endif /* __U_UAC3_H */
