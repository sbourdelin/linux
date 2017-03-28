
#ifndef _LINUX_VIRTIO_RPMSG_H
#define _LINUX_VIRTIO_RPMSG_H

/* Offset in struct fw_rsc_vdev */
#define RPMSG_CONFIG_OFFSET	0

/**
 * struct virtio_rpmsg_cfg - optional configuration field for virtio rpmsg
 * provided at probe time by virtio (get/set)
 * @id: virtio cfg id (as in virtio_ids.h)
 * @version: virtio_rpmsg_cfg structure version number
 * @da: device address
 * @pa: physical address
 * @len: length (in bytes)
 * @buf_size: size of rpmsg buffer size (defined by firmware else default value
 *	      used)
 * @reserved: reserved (must be zero)
 */
struct virtio_rpmsg_cfg {
	u32 id;
	u32 version;
	u32 da;
	u32 pa;
	u32 len;
	u32 buf_size;
	u32 reserved;
} __packed;

#endif
