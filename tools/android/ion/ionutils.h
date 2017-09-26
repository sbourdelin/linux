#ifndef __ION_UTILS_H
#define __ION_UTILS_H

#include "ion.h"

#define SOCKET_NAME "ion_socket"
#define ION_DEVICE "/dev/ion"

#define ION_BUFFER_LEN	32

struct socket_info {
	int sockfd;
	int datafd;
	unsigned long buflen;
};

struct ion_buffer_info {
	int ionfd;
	int buffd;
	unsigned int heap_type;
	unsigned int flag_type;
	unsigned long heap_size;
	unsigned long buflen;
	unsigned char *buffer;
	struct ion_handle_data ion_handle;
};


/* This is used to fill the data into the mapped buffer */
void write_buffer(void *buffer, unsigned long len);

/* This is used to read the data from the exported buffer */
void read_buffer(void *buffer, unsigned long len);

/* This is used to create an ION buffer FD for the kernel buffer */
/* So you can export this same buffer to others in the form of FD */
int ion_export_buffer_fd(struct ion_buffer_info *ion_info);

/* This is used to retrive an exported FD.
 * So we point to same buffer without making a copy. Hence zero-copy.
 */
int ion_import_buffer_fd(struct ion_buffer_info *ion_info);

/* This is used to close all references for the ION client */
void ion_close_buffer_fd(struct ion_buffer_info *ion_info);

/* This is used to send FD to another process using socket IPC */
int socket_send_fd(struct socket_info *skinfo);

/* This is used to receive FD from another process using socket IPC */
int socket_receive_fd(struct socket_info *skinfo);


#endif

