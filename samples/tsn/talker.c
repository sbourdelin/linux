/*
 * Copyright (c) 2017, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Intel Corporation nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <alloca.h>
#include <argp.h>
#include <arpa/inet.h>
#include <inttypes.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_FRAME_SIZE 1500

static uint8_t multicast_macaddr[] = { 0xBB, 0xAA, 0xBB, 0xAA, 0xBB, 0xAA };
static char ifname[IFNAMSIZ];
static int prio = -1;
static int arg_count;

static struct argp_option options[] = {
	{"ifname", 'i', "IFNAME", 0, "Network Interface" },
	{"prio", 'p', "NUM", 0, "SO_PRIORITY to be set in socket" },
	{ 0 }
};

static error_t parser(int key, char *arg, struct argp_state *s)
{
	switch (key) {
	case 'i':
		strncpy(ifname, arg, sizeof(ifname) - 1);
		arg_count++;
		break;
	case 'p':
		prio = atoi(arg);
		if (prio < 0)
			argp_failure(s, 1, 0, "Priority must be >=0\n");
		arg_count++;
		break;
	case ARGP_KEY_END:
		if (arg_count < 2)
			argp_failure(s, 1, 0,
				     "Options missing. Check --help\n");
		break;
	}

	return 0;
}

static struct argp argp = { options, parser };

int main(int argc, char *argv[])
{
	struct sockaddr_ll dst_ll_addr = {
		.sll_family = AF_PACKET,
		.sll_protocol = htons(ETH_P_TSN),
		.sll_halen = ETH_ALEN,
	};
	struct ifreq req;
	uint8_t *payload;
	int fd, res;

	argp_parse(&argp, argc, argv, 0, NULL, NULL);

	fd = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_TSN));
	if (fd < 0) {
		perror("Couldn't open socket");
		return 1;
	}

	strncpy(req.ifr_name, ifname, sizeof(req.ifr_name));
	res = ioctl(fd, SIOCGIFINDEX, &req);
	if (res < 0) {
		perror("Couldn't get interface index");
		goto err;
	}

	dst_ll_addr.sll_ifindex = req.ifr_ifindex;
	memcpy(&dst_ll_addr.sll_addr, multicast_macaddr, ETH_ALEN);

	res = setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));
	if (res < 0) {
		perror("Couldn't set priority");
		goto err;
	}

	payload = alloca(MAX_FRAME_SIZE);
	memset(payload, 0xBE, MAX_FRAME_SIZE);

	printf("Sending packets...\n");

	while (1) {
		ssize_t n = sendto(fd, payload, MAX_FRAME_SIZE, 0,
				(struct sockaddr *) &dst_ll_addr,
				sizeof(dst_ll_addr));

		if (n < 0)
			perror("Failed to send data");

		/* Sleep for 500us to avoid starvation from a 20Mbps stream. */
		usleep(500);
	}

err:
	close(fd);
	return 1;
}
