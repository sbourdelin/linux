/* f_acm.h -- Header file for USB CDC-ACM gadget function */

#ifndef __UAPI_LINUX_USB_F_ACM_H
#define __UAPI_LINUX_USB_F_ACM_H

#include <linux/usb/cdc.h>
#include <linux/ioctl.h>

/* The 0xCD code is also used by reiserfs. we use 0x10-0x1F range */
#define USB_F_ACM_GET_LINE_CODING _IOR(0xCD, 0x10, struct usb_cdc_line_coding)

#endif /* __UAPI_LINUX_USB_F_ACM_H */
