#include <linux/bpf.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <linux/checmate.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "bpf_load.h"
#include "libbpf.h"
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>

int main(int ac, char **argv)
{
	char filename[256];
	int rc = 0;
	int sockfd;
	struct sockaddr_in in_addr;

	snprintf(filename, sizeof(filename), "%s_kern.o", argv[0]);
	if (load_bpf_file(filename)) {
		printf("%s", bpf_log_buf);
		return 1;
	}
	if (!prog_fd[0]) {
		printf("load_bpf_file: %s\n", strerror(errno));
		return 1;
	}
	rc = prctl(PR_CHECMATE, CHECMATE_INSTALL_HOOK,
		   CHECMATE_HOOK_SOCKET_CONNECT, prog_fd[0]);
	if (rc) {
		printf("Failed to install hook: %s\n", strerror(errno));
		return 1;
	}
	assert((sockfd = socket(AF_INET, SOCK_STREAM, 0)) > 0);

	/* Fake destination address (on loopback) on port 1 */
	in_addr.sin_family = AF_INET;
	in_addr.sin_port = htons(1);
	in_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(connect(sockfd, (const struct sockaddr *)&in_addr,
		       sizeof(in_addr)) != 0);
	assert(errno = -EPERM);

	rc = prctl(PR_CHECMATE, CHECMATE_RESET,
		   CHECMATE_HOOK_SOCKET_CONNECT, prog_fd[0]);
	if (rc) {
		printf("Failed to reset hook: %s\n", strerror(errno));
		return 1;
	}

	return rc;
}
