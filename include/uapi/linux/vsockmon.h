#ifndef _UAPI_VSOCKMON_H
#define _UAPI_VSOCKMON_H

#include <linux/virtio_vsock.h>

/*
 * Structure of packets received through the vsockmon device.
 *
 * Note that after the vsockmon header comes the transport header (len bytes and
 * type specified by t) and if the packet op is AF_VSOCK_OP_PAYLOAD then comes
 * the payload.
 */

struct af_vsockmon_hdr {
	__le64 src_cid;
	__le64 dst_cid;
	__le32 src_port;
	__le32 dst_port;
	__le16 op;			/* enum af_vsockmon_op */
	__le16 t;			/* enum af_vosckmon_t */
	__le16 len;			/* Transport header length */
} __attribute__((packed));

enum af_vsockmon_op {
	AF_VSOCK_OP_UNKNOWN = 0,
	AF_VSOCK_OP_CONNECT = 1,
	AF_VSOCK_OP_DISCONNECT = 2,
	AF_VSOCK_OP_CONTROL = 3,
	AF_VSOCK_OP_PAYLOAD = 4,
};

enum af_vsockmon_t {
	AF_VSOCK_T_UNKNOWN = 0,
	AF_VSOCK_T_NO_INFO = 1,		/* No transport information */
	AF_VSOCK_T_VIRTIO = 2,		/* Virtio transport header (struct virtio_vsock_hdr) */
};

#endif
