/*
 * Copyright (C) 2017 Bitdefender S.R.L.
 *
 * The KVMI Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * The KVMI Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the GNU C Library; if not, see
 * <http://www.gnu.org/licenses/>
 */
#ifndef __KVMI_SOCKET_H__
#define __KVMI_SOCKET_H__

typedef int (*kvmi_socket_read_cb) (void *, void *buf, size_t len);
typedef bool(*kvmi_socket_use_cb) (void *ctx, kvmi_socket_read_cb read_cb,
				   void *read_ctx);

int kvmi_socket_start_vsock(unsigned int cid, unsigned int port,
			    kvmi_socket_use_cb cb, void *cb_ctx);
void kvmi_socket_stop(void);
void *kvmi_socket_monitor(void *s, kvmi_socket_use_cb cb, void *cb_ctx);
int kvmi_socket_send(void *s, struct kvec *i, size_t n, size_t size);
void kvmi_socket_release(void *s);
bool kvmi_socket_is_active(void *s);

#endif
