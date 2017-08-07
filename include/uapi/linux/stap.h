/*
 * Socket tap
 *
 * Copyright (c) 2017 Tom Herbert <tom@quantonium.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 */

#ifndef _UAPI_LINUX_STAP_H
#define _UAPI_LINUX_STAP_H

struct stap_params {
	int bpf_send_parse_fd;
	int bpf_send_verdict_fd;
	int bpf_recv_parse_fd;
	int bpf_recv_verdict_fd;
};

#endif /* _UAPI_LINUX_STAP_H */
