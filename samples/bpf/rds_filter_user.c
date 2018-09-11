// SPDX-License-Identifier: GPL-2.0
#include <arpa/inet.h>
#include <assert.h>
#include "bpf_load.h"
#include <getopt.h>
#include <errno.h>
#include <netinet/in.h>
#include <limits.h>
#include <linux/sockios.h>
#include <linux/rds.h>
#include <linux/errqueue.h>
#include <linux/bpf.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#define TESTPORT	4000
#define BUFSIZE		8192

int transport = -1;

static int str2trans(const char *trans)
{
	if (strcmp(trans, "tcp") == 0)
		return RDS_TRANS_TCP;
	if (strcmp(trans, "ib") == 0)
		return RDS_TRANS_IB;
	return (RDS_TRANS_NONE);
}

static const char *trans2str(int trans)
{
	switch (trans) {
	case RDS_TRANS_TCP:
		return ("tcp");
	case RDS_TRANS_IB:
		return ("ib");
	case RDS_TRANS_NONE:
		return ("none");
	default:
		return ("unknown");
	}
}

static int gettransport(int sock)
{
	int err;
	char val;
	socklen_t len = sizeof(int);

	err = getsockopt(sock, SOL_RDS, SO_RDS_TRANSPORT,
			 (char *)&val, &len);
	if (err < 0) {
		fprintf(stderr, "%s: getsockopt %s\n",
			__func__, strerror(errno));
		return err;
	}
	return (int)val;
}

static int settransport(int sock, int transport)
{
	int err;

	err = setsockopt(sock, SOL_RDS, SO_RDS_TRANSPORT,
			 (char *)&transport, sizeof(transport));
	if (err < 0) {
		fprintf(stderr, "could not set transport %s, %s\n",
			trans2str(transport), strerror(errno));
	}
	return err;
}

static void print_sock_local_info(int fd, char *str, struct sockaddr_in *ret)
{
	socklen_t sin_size = sizeof(struct sockaddr_in);
	struct sockaddr_in sin;
	int err;

	err = getsockname(fd, (struct sockaddr *)&sin, &sin_size);
	if (err < 0) {
		fprintf(stderr, "%s getsockname %s\n",
			__func__, strerror(errno));
		return;
	}
	printf("%s address: %s port %d\n",
		(str ? str : ""), inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));

	if (ret != NULL)
		*ret = sin;
}

static void print_payload(char *buf)
{
	int i;

	printf("payload contains:");
	for (i = 0; i < 10; i++)
		printf("%x ", buf[i]);
	printf("...\n");
}

static void server(char *address, in_port_t port)
{
	struct sockaddr_in sin, din;
	struct msghdr msg;
	struct iovec *iov;
	int rc, sock;
	char *buf;

	buf = calloc(BUFSIZE, sizeof(char));
	if (!buf) {
		fprintf(stderr, "%s: calloc %s\n", __func__, strerror(errno));
		return;
	}

	sock = socket(PF_RDS, SOCK_SEQPACKET, 0);
	if (sock < 0) {
		fprintf(stderr, "%s: socket %s\n", __func__, strerror(errno));
		goto out;
	}
	if (settransport(sock, transport) < 0)
		goto out;

	printf("transport %s\n", trans2str(gettransport(sock)));

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = inet_addr(address);
	sin.sin_port = htons(port);

	rc = bind(sock, (struct sockaddr *)&sin, sizeof(sin));
	if (rc < 0) {
		fprintf(stderr, "%s: bind %s\n", __func__, strerror(errno));
		goto out;
	}

	/* attach bpf prog */
	assert(setsockopt(sock, SOL_SOCKET, SO_ATTACH_BPF, prog_fd,
			  sizeof(prog_fd[0])) == 0);

	print_sock_local_info(sock, "server bound to", NULL);

	iov = calloc(1, sizeof(struct iovec));
	if (!iov) {
		fprintf(stderr, "%s: calloc %s\n", __func__, strerror(errno));
		goto out;
	}

	while (1) {
		memset(buf, 0, BUFSIZE);
		iov[0].iov_base = buf;
		iov[0].iov_len = BUFSIZE;

		memset(&msg, 0, sizeof(msg));
		msg.msg_name = &din;
		msg.msg_namelen = sizeof(din);
		msg.msg_iov = iov;
		msg.msg_iovlen = 1;

		printf("server listening on %s\n", inet_ntoa(sin.sin_addr));

		rc = recvmsg(sock, &msg, 0);
		if (rc < 0) {
			fprintf(stderr, "%s: recvmsg %s\n",
				__func__, strerror(errno));
			break;
		}

		printf("%s received a packet from %s of len %d cmsg len %d, on port %d\n",
			inet_ntoa(sin.sin_addr),
			inet_ntoa(din.sin_addr),
			(uint32_t) iov[0].iov_len,
			(uint32_t) msg.msg_controllen,
			ntohs(din.sin_port));

		print_payload(buf);
	}
	free(iov);
out:
	free(buf);
}

static void create_message(char *buf)
{
	unsigned int i;

	for (i = 0; i < BUFSIZE; i++) {
		buf[i] = i + 0x30;
	}
}

static int build_rds_packet(struct msghdr *msg, char *buf)
{
	struct iovec *iov;

	iov = calloc(1, sizeof(struct iovec));
	if (!iov) {
		fprintf(stderr, "%s: calloc %s\n", __func__, strerror(errno));
		return -1;
	}

	msg->msg_iov = iov;
	msg->msg_iovlen = 1;

	iov[0].iov_base = buf;
	iov[0].iov_len = BUFSIZE * sizeof(char);

	return 0;
}

static void client(char *localaddr, char *remoteaddr, in_port_t server_port)
{
	struct sockaddr_in sin, din;
	struct msghdr msg;
	int rc, sock;
	char *buf;

	buf = calloc(BUFSIZE, sizeof(char));
	if (!buf) {
		fprintf(stderr, "%s: calloc %s\n", __func__, strerror(errno));
		return;
	}

	create_message(buf);

	sock = socket(PF_RDS, SOCK_SEQPACKET, 0);
	if (sock < 0) {
		fprintf(stderr, "%s: socket %s\n", __func__, strerror(errno));
		goto out;
	}

	if (settransport(sock, transport) < 0)
		goto out;

	printf("transport %s\n", trans2str(gettransport(sock)));

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = inet_addr(localaddr);
	sin.sin_port = 0;

	rc = bind(sock, (struct sockaddr *)&sin, sizeof(sin));
	if (rc < 0) {
		fprintf(stderr, "%s: bind %s\n", __func__, strerror(errno));
		goto out;
	}
	print_sock_local_info(sock, "client bound to",  &sin);

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = &din;
	msg.msg_namelen = sizeof(din);

	memset(&din, 0, sizeof(din));
	din.sin_family = AF_INET;
	din.sin_addr.s_addr = inet_addr(remoteaddr);
	din.sin_port = htons(server_port);

	rc = build_rds_packet(&msg, buf);
	if (rc < 0)
		goto out;

	printf("client sending %d byte message from %s to %s on port %d\n",
		(uint32_t) msg.msg_iov->iov_len, localaddr,
		remoteaddr, ntohs(sin.sin_port));

	rc = sendmsg(sock, &msg, 0);
	if (rc < 0)
		fprintf(stderr, "%s: sendmsg %s\n", __func__, strerror(errno));

	print_payload(buf);

	if (msg.msg_control)
		free(msg.msg_control);
	if (msg.msg_iov)
		free(msg.msg_iov);
out:
	free(buf);

	return;
}

static void usage(char *progname)
{
	fprintf(stderr, "Usage %s [-s srvaddr] [-c clientaddr] [-t transport]"
		"\n", progname);
}

int main(int argc, char **argv)
{
	in_port_t server_port = TESTPORT;
	char *serveraddr = NULL;
	char *clientaddr = NULL;
	char filename[256];
	int opt;

	while ((opt = getopt(argc, argv, "s:c:t:")) != -1) {
		switch (opt) {
		case 's':
			serveraddr = optarg;
			break;
		case 'c':
			clientaddr = optarg;
			break;
		case 't':
			transport = str2trans(optarg);
			if (transport == RDS_TRANS_NONE) {
				fprintf(stderr,
					"unknown transport %s\n", optarg);
					usage(argv[0]);
					return (-1);
			}
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	snprintf(filename, sizeof(filename), "%s_kern.o", argv[0]);

	if (load_bpf_file(filename)) {
		fprintf(stderr, "Error: load_bpf_file %s", bpf_log_buf);
		return 1;
	}

	if (serveraddr && !clientaddr) {
		printf("running server in a loop\n");
		server(serveraddr, server_port);
	} else if (serveraddr && clientaddr) {
		client(clientaddr, serveraddr, server_port);
	}

	return 0;
}
