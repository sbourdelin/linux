/* SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause) */
/*
 * Copyright © 2017-2018 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 *
 * Authors:
 *	Ramalingam C <ramalingam.c@intel.com>
 */

#ifndef _LINUX_MEI_HDCP_H
#define _LINUX_MEI_HDCP_H

enum mei_cldev_state {
	MEI_CLDEV_DISABLED,
	MEI_CLDEV_ENABLED
};

#if IS_ENABLED(CONFIG_INTEL_MEI_HDCP)
int mei_cldev_register_notify(struct notifier_block *nb);
int mei_cldev_unregister_notify(struct notifier_block *nb);
int mei_cldev_poll_notification(void);
#else
static inline int mei_cldev_register_notify(struct notifier_block *nb)
{
	return -ENODEV;
}
static inline int mei_cldev_unregister_notify(struct notifier_block *nb)
{
	return -ENODEV;
}
static inline int mei_cldev_poll_notification(void)
{
	return -ENODEV;
}
#endif /* IS_ENABLED(CONFIG_INTEL_MEI_HDCP) */
#endif /* defined (_LINUX_MEI_HDCP_H) */
