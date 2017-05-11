#ifndef UAPI_UFS_IOCTL_H_
#define UAPI_UFS_IOCTL_H_

#include <linux/types.h>

/*
 *  IOCTL opcode for ufs queries has the following opcode after
 *  SCSI_IOCTL_GET_PCI
 */
#define UFS_IOCTL_QUERY			0x53A0
#define UFS_IOCTL_AUTO_HIBERN8		0x53A1
#define UFS_IOCTL_TASK_MANAGEMENT	0x53A2
#define UFS_IOCTL_DME			0x53A3

/**
 * struct ufs_ioctl_query_data - used to transfer data to and from user via
 * ioctl
 *
 * @opcode: type of data to query (descriptor/attribute/flag)
 * @idn: id of the data structure
 * @buf_size: number of allocated bytes/data size on return
 * @buffer: data location
 *
 * Received: buffer and buf_size (available space for transferred data)
 * Submitted: opcode, idn, length, buf_size
 * Optionally submitted: buffer, buf_size (in Write operations)
 */
struct ufs_ioctl_query_data {
	/*
	 * User should select one of the opcode defined in "enum query_opcode".
	 * Please check include/uapi/scsi/ufs/ufs.h for the definition of it.
	 * Note that only UPIU_QUERY_OPCODE_READ_DESC,
	 * UPIU_QUERY_OPCODE_READ_ATTR & UPIU_QUERY_OPCODE_READ_FLAG are
	 * supported as of now. All other query_opcode would be considered
	 * invalid.
	 * As of now only read query operations are supported.
	 */
	__u32 opcode;
	/*
	 * User should select one of the idn from "enum flag_idn" or "enum
	 * attr_idn" or "enum desc_idn" based on whether opcode above is
	 * attribute, flag or descriptor.
	 * Please check include/uapi/scsi/ufs/ufs.h for the definition of it.
	 */
	__u8 idn;
	/*
	 * User should specify the size of the buffer (buffer[0] below) where
	 * it wants to read the query data (attribute/flag/descriptor).
	 * As we might end up reading less data then what is specified in
	 * buf_size. So we are updating buf_size to what exactly we have read.
	 */
	__u16 buf_size;
	/*
	 * Pointer to the the data buffer where kernel will copy
	 * the query data (attribute/flag/descriptor) read from the UFS device
	 * Note:
	 * For Read Descriptor you will have to allocate at the most 255 bytes
	 * For Read Attribute you will have to allocate 4 bytes
	 * For Read Flag you will have to allocate 1 byte
	 */
	__u8 *buffer;
};

/**
 * struct ufs_ioctl_auto_hibern8_data - used to hold Auto-Hibern8 feature
 * configuration
 *
 * @write: flag indicating whether config should be written or read
 * @scale: scale of the timer (length of one tick)
 * @timer_val: value of the timer to be multipled by scale (0x0000-0x3FFF)
 *
 * Received/Submitted: scale, timer_val
 */
struct ufs_ioctl_auto_hibern8_data {
	/*
	 * This flag indicates whether configuration wirtten in this structure
	 * should be written, or overwritten by reading currently written
	 */
	bool write;

	/*
	 * Scale of the timer. Prease refer to <uapi/scsi/ufs/ufshci.h> for
	 * correct values and their meaning.
	 */
	__u8 scale;

	/*
	 * Actual timer value, which will be multipled by the scale.
	 * Maximal value: 1023. 0 will disable the feature.
	 */
	__u16 timer_val;
};

/**
 * struct ufs_ioctl_task_mgmt_data - used to perform Task Management specific
 * functions
 *
 * @task_id: ID of a task to be managed
 * @task_func: function to perform on managed task
 * @response: Task Management response
 *
 * Submitted: task_id, task_func
 * Received: response
 */
struct ufs_ioctl_task_mgmt_data {
	__u8 task_id;
	__u8 task_func;
	__u8 response;
};

/**
 * struct ufs_ioctl_dme_get_data - used to request information from a specific
 * UniPro or M-PHY attribute
 *
 * @attr_id: attribute identifier (valid range: 0x0000 to 0x7FFF)
 * @selector: selector on attribute ( shall be != 0 only for some attribiutes;
 * please reffer UniPro documentation for details)
 * @peer: indicate whether peer or local UniPro Link
 * @response: variable where the kernel will put the attribute value read from
 * the local or peer UniPro Link
 *
 * Submitted: attr_id, peer
 * Received: response
 */
struct ufs_ioctl_dme_get_data {
	__u16 attr_id;
	__u16 selector;
	bool peer;
	__u32 response;
};

#endif /* UAPI_UFS_IOCTL_H_ */
