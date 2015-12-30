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

#include <sys/socket.h>

#include <string.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "usbip_common.h"
#include "usbip_network.h"

/*
 * IPv6 Ready
 */
static usbip_sock_t *net_tcp_open(const char *hostname, const char *service)
{
	struct addrinfo hints, *res, *rp;
	int sockfd;
	usbip_sock_t *sock;
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	/* get all possible addresses */
	ret = getaddrinfo(hostname, service, &hints, &res);
	if (ret < 0) {
		dbg("getaddrinfo: %s service %s: %s", hostname, service,
		    usbip_net_gai_strerror(ret));
		return NULL;
	}

	/* try the addresses */
	for (rp = res; rp; rp = rp->ai_next) {
		sockfd = socket(rp->ai_family, rp->ai_socktype,
				rp->ai_protocol);
		if (sockfd < 0)
			continue;

		/* should set TCP_NODELAY for usbip */
		usbip_net_set_nodelay(sockfd);
		/* TODO: write code for heartbeat */
		usbip_net_set_keepalive(sockfd);

		if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
			break;

		close(sockfd);
	}

	freeaddrinfo(res);

	if (!rp)
		return NULL;

	sock = (usbip_sock_t*)malloc(sizeof(usbip_sock_t));
	if (!sock) {
		dbg("Fail to malloc usbip_sock");
		close(sockfd);
		return NULL;
	}
	usbip_sock_init(sock, sockfd, NULL, NULL, NULL, NULL);

	return sock;
}

static void net_tcp_close(usbip_sock_t *sock)
{
	close(sock->fd);
	free(sock);
}

void usbip_net_tcp_conn_init()
{
	usbip_conn_init(net_tcp_open, net_tcp_close);
}

