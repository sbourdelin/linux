/*
 * Copyright (C) 2015 Nobuo Iwata
 *               2011 matt mooney <mfm@muteddisk.com>
 *               2005-2007 Takahiro Hirofuchi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __USBIPD_H
#define __USBIPD_H

#include "usbip_common.h"
#include "usbip_ux.h"

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

extern char *usbip_progname;
extern char *usbip_default_pid_file;

int usbip_recv_pdu(usbip_sock_t *sock, const char *host, const char *port);
inline void usbip_break_connections(void) { usbip_ux_interrupt_pgrp(); }
int usbip_driver_open(void);
void usbip_driver_close(void);

#endif /* __USBIPD_H */
