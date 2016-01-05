/*
 * Copyright (C) 2015 Nobuo Iwata
 *
 * USB/IP URB transmission in userspace.
 */

#ifndef __USBIP_UX_H
#define __USBIP_UX_H

#include <pthread.h>
#include <linux/usbip_ux.h>
#include "usbip_common.h"

typedef struct usbip_ux {
	int sockfd;
	int devfd;
	int started;
	pthread_t tx, rx;
	struct usbip_ux_kaddr kaddr;
} usbip_ux_t;

int usbip_ux_setup(int sockfd, usbip_ux_t **uxp);
void usbip_ux_cleanup(usbip_ux_t **ux);
int usbip_ux_start(usbip_ux_t *ux);
void usbip_ux_join(usbip_ux_t *ux);
void usbip_ux_interrupt(usbip_ux_t *ux);
void usbip_ux_interrupt_pgrp();
int usbip_ux_installed(void);

#endif /* !__USBIP_UX_H */
