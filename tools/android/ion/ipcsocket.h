
#ifndef _IPCSOCKET_H
#define _IPCSOCKET_H


#define MAX_SOCK_NAME_LEN	64

char sock_name[MAX_SOCK_NAME_LEN];

/* This structure is responsible for holding the IPC data
 * data: hold the buffer fd
 * len: just the length of 32-bit integer fd
 */
struct socketdata {
	int data;
	unsigned int len;
};


int opensocket(int *sockfd, const char *name, int connecttype);
int sendtosocket(int sockfd, struct socketdata *data);
int receivefromsocket(int sockfd, struct socketdata *data);
int closesocket(int sockfd, char *name);


#endif

