/*
 *  History:
 *  Started: Aug 9 by Lawrence Foard (entropy@world.std.com),
 *           to allow user process control of SCSI devices.
 *  Development Sponsored by Killy Corp. NY NY
 *
 * Original driver (sg.c):
 *        Copyright (C) 1992 Lawrence Foard
 * Version 2 and 3 extensions to driver:
 *        Copyright (C) 1998 - 2018 Douglas Gilbert
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 */

static int sg_version_num = 30901;	/* 2 digits for each component */
#define SG_VERSION_STR "3.9.01"

#include <linux/module.h>

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/mtio.h>
#include <linux/ioctl.h>
#include <linux/slab.h>
#include <linux/fcntl.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/moduleparam.h>
#include <linux/cdev.h>
#include <linux/idr.h>
#include <linux/seq_file.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/blktrace_api.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/ratelimit.h>
#include <linux/uio.h>
#include <linux/cred.h> /* for sg_check_file_access() */
#include <linux/bsg.h>
#include <linux/timekeeping.h>

#include "scsi.h"
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_ioctl.h>
#include <scsi/sg.h>

#include "scsi_logging.h"

#ifdef CONFIG_SCSI_PROC_FS
#include <linux/proc_fs.h>
static char *sg_version_date = "20181018";

static int sg_proc_init(void);
#endif

#define SG_ALLOW_DIO_DEF 0

#define SG_MAX_DEVS 32768

/*
 * SG_MAX_CDB_SIZE should be 260 (spc4r37 section 3.1.30) however the type
 * of sg_io_hdr::cmd_len can only represent 255. All SCSI commands greater
 * than 16 bytes are "variable length" whose length is a multiple of 4
 */
#define SG_MAX_CDB_SIZE 252

/* Following defines are states of sg_request::rq_state */
#define SG_RQ_INACTIVE 0        /* request not in use (e.g. on fl) */
#define SG_RQ_INFLIGHT 1        /* SCSI request issued, no response yet */
#define SG_RQ_AWAIT_READ 2      /* response received, awaiting read */
#define SG_RQ_DONE_READ 3       /* read is ongoing or done */
#define SG_RQ_BUSY 4            /* example: reserve request changing size */

/* free up requests larger than this dlen size after use */
#define SG_RQ_DATA_THRESHOLD (128 * 1024)

/* If sum_of(dlen) of a fd exceeds this, write() will yield E2BIG */
#define SG_TOT_FD_THRESHOLD (16 * 1024 * 1024)

#define SG_TIME_UNIT_MS 0       /* milliseconds */
#define SG_TIME_UNIT_NS 1       /* nanoseconds */
#define SG_DEF_TIME_UNIT SG_TIME_UNIT_MS
#define SG_DEFAULT_TIMEOUT mult_frac(SG_DEFAULT_TIMEOUT_USER, HZ, USER_HZ)

int sg_big_buff = SG_DEF_RESERVED_SIZE;
/* N.B. This variable is readable and writeable via
   /proc/scsi/sg/def_reserved_size . Each time sg_open() is called a buffer
   of this size (or less if there is not enough memory) will be reserved
   for use by this file descriptor. [Deprecated usage: this variable is also
   readable via /proc/sys/kernel/sg-big-buff if the sg driver is built into
   the kernel (i.e. it is not a module).] */
static int def_reserved_size = -1;	/* picks up init parameter */
static int sg_allow_dio = SG_ALLOW_DIO_DEF;

static int scatter_elem_sz = SG_SCATTER_SZ;
static int scatter_elem_sz_prev = SG_SCATTER_SZ;

#define SG_SECTOR_SZ 512

static int sg_add_device(struct device *, struct class_interface *);
static void sg_remove_device(struct device *, struct class_interface *);

static DEFINE_IDR(sg_index_idr);
static DEFINE_RWLOCK(sg_index_lock); /* Also used to lock fd list for device */

static struct class_interface sg_interface = {
	.add_dev        = sg_add_device,
	.remove_dev     = sg_remove_device,
};

struct sg_v4_hold {	/* parts of sg_io_v4 object needed in async usage */
	__user u8 *usr_ptr;	/* derived from sg_io_v4::usr_ptr */
	__user u8 *sbp;		/* derived from sg_io_v4::response */
	u16 cmd_len;		/* truncated of sg_io_v4::request_len */
	u16 max_sb_len;		/* truncated of sg_io_v4::max_response_len */
	u32 flags;		/* copy of sg_io_v4::flags */
};

struct sg_scatter_hold {     /* holding area for scsi scatter gather info */
	struct page **pages;	/* num_sgat element array of struct page* */
	int page_order;		/* byte_len = (page_size * (2**page_order)) */
	int dlen;		/* Byte length of data buffer */
	unsigned short num_sgat;/* actual number of scatter-gather segments */
	bool dio_in_use;	/* false->indirect IO (or mmap), true->dio */
	u8 cmd_opcode;		/* first byte of command */
};

struct sg_device;		/* forward declarations */
struct sg_fd;

struct sg_request {	/* SG_MAX_QUEUE requests outstanding per file */
	struct list_head entry;	/* list entry */
	struct sg_fd *parentfp;	/* NULL -> not in use */
	struct sg_scatter_hold data;	/* hold buffer, perhaps scatter list */
	union {
		struct sg_io_hdr header;  /* see <scsi/sg.h> */
		struct sg_io_v4 hdr_v4;   /* see <uapi/linux/bsg.h> */
	};
	u8 sense_b[SCSI_SENSE_BUFFERSIZE];
	bool hdr_v4_active;	/* selector for anonymous union above */
	bool res_used;	/* true -> use reserve buffer, false -> don't */
	bool orphan;	/* true -> drop on sight, false -> normal */
	bool sg_io_owned;	/* true -> packet belongs to SG_IO */
	/* done protected by rq_list_lock */
	char done;		/* 0->before bh, 1->before read, 2->read */
	struct request *rq;
	struct bio *bio;
	struct execute_work ew;
};

struct sg_fd {			/* holds the state of a file descriptor */
	struct list_head sfd_siblings;  /* protected by device's sfd_lock */
	struct sg_device *parentdp;	/* owning device */
	wait_queue_head_t read_wait;	/* queue read until command done */
	rwlock_t rq_list_lock;	/* protect access to list in req_arr */
	struct mutex f_mutex;	/* protect against changes in this fd */
	int timeout;		/* defaults to SG_DEFAULT_TIMEOUT      */
	int timeout_user;	/* defaults to SG_DEFAULT_TIMEOUT_USER */
	struct sg_scatter_hold reserve;	/* one held for this file descriptor */
	struct list_head rq_list; /* head of request list */
	struct fasync_struct *async_qp;	/* used by asynchronous notification */
	struct sg_request req_arr[SG_MAX_QUEUE];/* used as singly-linked list */
	bool force_packid;	/* true -> pack_id input to read() */
	bool cmd_q;	/* true -> allow command queuing, false -> don't */
	u8 next_cmd_len;	/* 0: automatic, >0: use on next write() */
	bool keep_orphan;/* false -> drop (def), true -> keep for read() */
	bool mmap_called;	/* false -> mmap() never called on this fd */
	bool res_in_use;	/* true -> 'reserve' array in use */
	struct kref f_ref;
	struct execute_work ew;
};

struct sg_device {	/* holds the state of each scsi generic device */
	struct scsi_device *device;
	wait_queue_head_t open_wait;    /* queue open() when O_EXCL present */
	struct mutex open_rel_lock;     /* held when in open() or release() */
	int sg_tablesize;	/* adapter's max scatter-gather table size */
	u32 index;		/* device index number */
	struct list_head sfds;
	rwlock_t sfd_lock;      /* protect access to sfd list */
	atomic_t detaching;     /* 0->device usable, 1->device detaching */
	bool exclude;		/* 1->open(O_EXCL) succeeded and is active */
	int open_cnt;		/* count of opens (perhaps < num(sfds) ) */
	char sgdebug;		/* 0->off, 1->sense, 9->dump dev, 10-> all devs */
	struct gendisk *disk;
	struct cdev * cdev;	/* char_dev [sysfs: /sys/cdev/major/sg<n>] */
	struct kref d_ref;
};

/* tasklet or soft irq callback */
static void sg_rq_end_io(struct request *rq, blk_status_t status);
static int sg_start_req(struct sg_request *srp, u8 *cmd);
static int sg_finish_rem_req(struct sg_request *srp);
static int sg_build_indirect(struct sg_scatter_hold *schp, struct sg_fd *sfp,
			     int buff_size);
static ssize_t sg_new_read(struct sg_fd *sfp, char __user *buf, size_t count,
			   struct sg_request *srp);
static ssize_t sg_new_write(struct sg_fd *sfp, struct file *file,
			    const char __user *buf, size_t count, int blocking,
			    int read_only, int sg_io_owned,
			    struct sg_request **o_srp);
static int sg_common_write(struct sg_fd *sfp, struct sg_request *srp,
			   u8 *cmnd, int timeout, int blocking);
static int sg_read_oxfer(struct sg_request *srp, char __user *outp,
			 int num_read_xfer);
static void sg_remove_scat(struct sg_fd *sfp, struct sg_scatter_hold *schp);
static void sg_build_reserve(struct sg_fd *sfp, int req_size);
static void sg_link_reserve(struct sg_fd *sfp, struct sg_request *srp,
			    int size);
static void sg_unlink_reserve(struct sg_fd *sfp, struct sg_request *srp);
static struct sg_fd *sg_add_sfp(struct sg_device *sdp);
static void sg_remove_sfp(struct kref *);
static struct sg_request *sg_get_rq_pack_id(struct sg_fd *sfp, int pack_id);
static struct sg_request *sg_add_request(struct sg_fd *sfp);
static int sg_remove_request(struct sg_fd *sfp, struct sg_request *srp);
static struct sg_device *sg_get_dev(int dev);
static void sg_device_destroy(struct kref *kref);

#define SZ_SG_HEADER sizeof(struct sg_header)
#define SZ_SG_IO_HDR sizeof(struct sg_io_hdr)
/* #define SZ_SG_IOVEC sizeof(struct sg_iovec) synonym for 'struct iovec' */
#define SZ_SG_REQ_INFO sizeof(struct sg_req_info)

/*
 * Kernel needs to be built with CONFIG_SCSI_LOGGING to see log messages.
 * 'depth' is a number between 1 (most severe) and 7 (most noisy, most
 * information). All messages are logged as informational (KERN_INFO). In
 * the unexpected situation where sdp is NULL the macro reverts to a pr_info
 * and ignores CONFIG_SCSI_LOGGING and always prints to the log.
 */
#define SG_LOG(depth, sdp, fmt, a...)				\
	do {								\
		if (IS_ERR_OR_NULL(sdp)) {				\
			pr_info("sg: sdp=NULL_or_ERR, " fmt, ##a);	\
		} else {						\
			SCSI_LOG_TIMEOUT(depth, sdev_prefix_printk(	\
					 KERN_INFO, (sdp)->device,	\
					 (sdp)->disk->disk_name, fmt,	\
					 ##a));				\
		}							\
	} while (0)

/*
 * The SCSI interfaces that use read() and write() as an asynchronous variant of
 * ioctl(..., SG_IO, ...) are fundamentally unsafe, since there are lots of ways
 * to trigger read() and write() calls from various contexts with elevated
 * privileges. This can lead to kernel memory corruption (e.g. if these
 * interfaces are called through splice()) and privilege escalation inside
 * userspace (e.g. if a process with access to such a device passes a file
 * descriptor to a SUID binary as stdin/stdout/stderr).
 *
 * This function provides protection for the legacy API by restricting the
 * calling context.
 *
 * N.B. In this driver EACCES is used when the caller does not have sufficient
 * privilege (e.g. not the root user) while EPERM indicates what has been
 * requested cannot be done, even if the root user is the caller.
 */
static int
sg_check_file_access(struct file *filp, const char *caller)
{
	if (filp->f_cred != current_real_cred()) {
		pr_err_once("%s: process %d (%s) changed security contexts after opening file descriptor, this is not allowed.\n",
			caller, task_tgid_vnr(current), current->comm);
		return -EPERM;
	}
	if (uaccess_kernel()) {
		pr_err_once("%s: process %d (%s) called from kernel context, this is not allowed.\n",
			caller, task_tgid_vnr(current), current->comm);
		return -EACCES;
	}
	return 0;
}

static int
sg_allow_access(struct file *filp, u8 *cmd)
{
	struct sg_fd *sfp = filp->private_data;

	if (sfp->parentdp->device->type == TYPE_SCANNER)
		return 0;

	return blk_verify_command(cmd, filp->f_mode);
}

static int
open_wait(struct sg_device *sdp, int flags)
{
	int retval = 0;

	if (flags & O_EXCL) {
		while (sdp->open_cnt > 0) {
			mutex_unlock(&sdp->open_rel_lock);
			retval = wait_event_interruptible(sdp->open_wait,
					(atomic_read(&sdp->detaching) ||
					 !sdp->open_cnt));
			mutex_lock(&sdp->open_rel_lock);

			if (retval) /* -ERESTARTSYS */
				return retval;
			if (atomic_read(&sdp->detaching))
				return -ENODEV;
		}
	} else {
		while (sdp->exclude) {
			mutex_unlock(&sdp->open_rel_lock);
			retval = wait_event_interruptible(sdp->open_wait,
					(atomic_read(&sdp->detaching) ||
					 !sdp->exclude));
			mutex_lock(&sdp->open_rel_lock);

			if (retval) /* -ERESTARTSYS */
				return retval;
			if (atomic_read(&sdp->detaching))
				return -ENODEV;
		}
	}

	return retval;
}

/* Returns 0 on success, else a negated errno value */
static int
sg_open(struct inode *inode, struct file *filp)
{
	int min_dev = iminor(inode);
	int flags = filp->f_flags;
	struct request_queue *q;
	struct sg_device *sdp;
	struct sg_fd *sfp;
	int retval;

	nonseekable_open(inode, filp);
	if ((flags & O_EXCL) && (O_RDONLY == (flags & O_ACCMODE)))
		return -EPERM; /* Can't lock it with read only access */
	sdp = sg_get_dev(min_dev);
	if (IS_ERR(sdp))
		return PTR_ERR(sdp);
	SG_LOG(3, sdp, "%s: flags=0x%x; device open count prior=%d\n",
	       __func__, flags, sdp->open_cnt);

	/* This driver's module count bumped by fops_get in <linux/fs.h> */
	/* Prevent the device driver from vanishing while we sleep */
	retval = scsi_device_get(sdp->device);
	if (retval)
		goto sg_put;

	retval = scsi_autopm_get_device(sdp->device);
	if (retval)
		goto sdp_put;

	/* scsi_block_when_processing_errors() may block so bypass
	 * check if O_NONBLOCK. Permits SCSI commands to be issued
	 * during error recovery. Tread carefully. */
	if (!((flags & O_NONBLOCK) ||
	      scsi_block_when_processing_errors(sdp->device))) {
		retval = -ENXIO;
		/* we are in error recovery for this device */
		goto error_out;
	}

	mutex_lock(&sdp->open_rel_lock);
	if (flags & O_NONBLOCK) {
		if (flags & O_EXCL) {
			if (sdp->open_cnt > 0) {
				retval = -EBUSY;
				goto error_mutex_locked;
			}
		} else {
			if (sdp->exclude) {
				retval = -EBUSY;
				goto error_mutex_locked;
			}
		}
	} else {
		retval = open_wait(sdp, flags);
		if (retval) /* -ERESTARTSYS or -ENODEV */
			goto error_mutex_locked;
	}

	/* N.B. at this point we are holding the open_rel_lock */
	if (flags & O_EXCL)
		sdp->exclude = true;

	if (sdp->open_cnt < 1) {  /* no existing opens */
		sdp->sgdebug = 0;
		q = sdp->device->request_queue;
		sdp->sg_tablesize = queue_max_segments(q);
	}
	sfp = sg_add_sfp(sdp);
	if (IS_ERR_OR_NULL(sfp)) {
		retval = IS_ERR(sfp) ? PTR_ERR(sfp) : -ENXIO;
		goto out_undo;
	}

	filp->private_data = sfp;
	sdp->open_cnt++;
	mutex_unlock(&sdp->open_rel_lock);

	retval = 0;
sg_put:
	kref_put(&sdp->d_ref, sg_device_destroy);
	return retval;

out_undo:
	if (flags & O_EXCL) {
		sdp->exclude = false;   /* undo if error */
		wake_up_interruptible(&sdp->open_wait);
	}
error_mutex_locked:
	mutex_unlock(&sdp->open_rel_lock);
error_out:
	scsi_autopm_put_device(sdp->device);
sdp_put:
	scsi_device_put(sdp->device);
	goto sg_put;
}

/*
 * Release resources associated with a prior, successful sg_open(). It can
 * be seen as the (final) close() call on a sg device file descriptor in the
 * user space. Returns 0 on success, else a negated errno value.
 */
static int
sg_release(struct inode *inode, struct file *filp)
{
	struct sg_device *sdp;
	struct sg_fd *sfp;

	sfp = (struct sg_fd *)filp->private_data;
	if (IS_ERR_OR_NULL(sfp)) {
		pr_warn("sg: %s: sfp is NULL or error\n", __func__);
		return IS_ERR(sfp) ? PTR_ERR(sfp) : -ENXIO;
	}
	sdp = sfp->parentdp;
	if (IS_ERR_OR_NULL(sdp))
		return IS_ERR(sdp) ? PTR_ERR(sdp) : -ENXIO;
	SG_LOG(3, sdp, "%s: device open count prior=%d\n", __func__,
	       sdp->open_cnt);

	mutex_lock(&sdp->open_rel_lock);
	scsi_autopm_put_device(sdp->device);
	kref_put(&sfp->f_ref, sg_remove_sfp);
	sdp->open_cnt--;

	/* possibly many open()s waiting on exlude clearing, start many;
	 * only open(O_EXCL)s wait on 0==open_cnt so only start one */
	if (sdp->exclude) {
		sdp->exclude = false;
		wake_up_interruptible_all(&sdp->open_wait);
	} else if (0 == sdp->open_cnt) {
		wake_up_interruptible(&sdp->open_wait);
	}
	mutex_unlock(&sdp->open_rel_lock);
	return 0;
}

static ssize_t
sg_read(struct file *filp, char __user *buf, size_t count, loff_t * ppos)
{
	struct sg_device *sdp;
	struct sg_fd *sfp;
	struct sg_request *srp;
	int req_pack_id = -1;
	struct sg_io_hdr *hp;
	struct sg_header *ohdr = NULL;
	int retval = 0;

	/*
	 * This could cause a response to be stranded. Close the associated
	 * file descriptor to free up any resources being held.
	 */
	retval = sg_check_file_access(filp, __func__);
	if (retval)
		return retval;

	sfp = (struct sg_fd *)filp->private_data;
	if (IS_ERR_OR_NULL(sfp)) {
		pr_warn("sg: %s: sfp is NULL or error\n", __func__);
		return IS_ERR(sfp) ? PTR_ERR(sfp) : -ENXIO;
	}
	sdp = sfp->parentdp;
	if (IS_ERR_OR_NULL(sdp))
		return IS_ERR(sdp) ? PTR_ERR(sdp) : -ENXIO;
	SG_LOG(3, sdp, "%s: read() count=%d\n", __func__, (int)count);

	if (!access_ok(VERIFY_WRITE, buf, count))
		return -EFAULT;
	if (sfp->force_packid && (count >= SZ_SG_HEADER)) {
		ohdr = kmalloc(SZ_SG_HEADER, GFP_KERNEL);
		if (!ohdr)
			return -ENOMEM;
		/* even though this is a read(), this code is cheating */
		if (__copy_from_user(ohdr, buf, SZ_SG_HEADER)) {
			retval = -EFAULT;
			goto free_old_hdr;
		}
		if (ohdr->reply_len < 0) {
			if (count >= SZ_SG_IO_HDR) {
				struct sg_io_hdr *new_hdr;

				new_hdr = kmalloc(SZ_SG_IO_HDR, GFP_KERNEL);
				if (!new_hdr) {
					retval = -ENOMEM;
					goto free_old_hdr;
				}
				retval =__copy_from_user
				    (new_hdr, buf, SZ_SG_IO_HDR);
				req_pack_id = new_hdr->pack_id;
				kfree(new_hdr);
				if (retval) {
					retval = -EFAULT;
					goto free_old_hdr;
				}
			}
		} else
			req_pack_id = ohdr->pack_id;
	}
	srp = sg_get_rq_pack_id(sfp, req_pack_id);
	if (!srp) {	/* nothing available so wait on packet to arrive */
		if (atomic_read(&sdp->detaching)) {
			retval = -ENODEV;
			goto free_old_hdr;
		}
		if (filp->f_flags & O_NONBLOCK) {
			retval = -EAGAIN;
			goto free_old_hdr;
		}
		retval = wait_event_interruptible(sfp->read_wait,
			(atomic_read(&sdp->detaching) ||
			(srp = sg_get_rq_pack_id(sfp, req_pack_id))));
		if (atomic_read(&sdp->detaching)) {
			retval = -ENODEV;
			goto free_old_hdr;
		}
		if (retval) {
			/* -ERESTARTSYS as signal hit process */
			goto free_old_hdr;
		}
	}
	if (srp->header.interface_id != '\0') {
		retval = sg_new_read(sfp, buf, count, srp);
		goto free_old_hdr;
	}

	hp = &srp->header;
	if (!ohdr) {
		ohdr = kmalloc(SZ_SG_HEADER, GFP_KERNEL);
		if (!ohdr) {
			retval = -ENOMEM;
			goto free_old_hdr;
		}
	}
	memset(ohdr, 0, SZ_SG_HEADER);
	ohdr->reply_len = (int)hp->timeout;
	ohdr->pack_len = ohdr->reply_len; /* old, strange behaviour */
	ohdr->pack_id = hp->pack_id;
	ohdr->twelve_byte = (srp->data.cmd_opcode >= 0xc0 &&
				hp->cmd_len == 12);
	ohdr->target_status = hp->masked_status;
	ohdr->host_status = hp->host_status;
	ohdr->driver_status = hp->driver_status;
	if ((CHECK_CONDITION & hp->masked_status) ||
	    (DRIVER_SENSE & hp->driver_status))
		memcpy(ohdr->sense_buffer, srp->sense_b,
		       sizeof(ohdr->sense_buffer));
	switch (hp->host_status) {
	/* This setup of 'result' is for backward compatibility and is best
	   ignored by the user who should use target, host + driver status */
	case DID_OK:
	case DID_PASSTHROUGH:
	case DID_SOFT_ERROR:
		ohdr->result = 0;
		break;
	case DID_NO_CONNECT:
	case DID_BUS_BUSY:
	case DID_TIME_OUT:
		ohdr->result = EBUSY;
		break;
	case DID_BAD_TARGET:
	case DID_ABORT:
	case DID_PARITY:
	case DID_RESET:
	case DID_BAD_INTR:
		ohdr->result = EIO;
		break;
	case DID_ERROR:
		ohdr->result = (srp->sense_b[0] == 0 &&
				hp->masked_status == GOOD) ? 0 : EIO;
		break;
	default:
		ohdr->result = EIO;
		break;
	}

	/* Now copy the result back to the user buffer.  */
	if (count >= SZ_SG_HEADER) {
		if (__copy_to_user(buf, ohdr, SZ_SG_HEADER)) {
			retval = -EFAULT;
			goto free_old_hdr;
		}
		buf += SZ_SG_HEADER;
		if (count > ohdr->reply_len)
			count = ohdr->reply_len;
		if (count > SZ_SG_HEADER) {
			if (sg_read_oxfer(srp, buf, count - SZ_SG_HEADER)) {
				retval = -EFAULT;
				goto free_old_hdr;
			}
		}
	} else
		count = (ohdr->result == 0) ? 0 : -EIO;
	sg_finish_rem_req(srp);
	sg_remove_request(sfp, srp);
	retval = count;
free_old_hdr:
	kfree(ohdr);
	return retval;
}

static ssize_t
sg_new_read(struct sg_fd *sfp, char __user *buf, size_t count,
	    struct sg_request *srp)
{
	int err = 0, err2;
	int len;
	struct sg_io_hdr *hp = &srp->header;

	if (count < SZ_SG_IO_HDR) {
		err = -EINVAL;
		goto err_out;
	}
	hp->sb_len_wr = 0;
	if ((hp->mx_sb_len > 0) && hp->sbp) {
		if ((CHECK_CONDITION & hp->masked_status) ||
		    (DRIVER_SENSE & hp->driver_status)) {
			int sb_len = SCSI_SENSE_BUFFERSIZE;
			sb_len = (hp->mx_sb_len > sb_len) ? sb_len : hp->mx_sb_len;
			/* Additional sense length field */
			len = 8 + (int)srp->sense_b[7];
			len = (len > sb_len) ? sb_len : len;
			if (copy_to_user(hp->sbp, srp->sense_b, len)) {
				err = -EFAULT;
				goto err_out;
			}
			hp->sb_len_wr = len;
		}
	}
	if (hp->masked_status || hp->host_status || hp->driver_status)
		hp->info |= SG_INFO_CHECK;
	if (copy_to_user(buf, hp, SZ_SG_IO_HDR)) {
		err = -EFAULT;
		goto err_out;
	}
err_out:
	err2 = sg_finish_rem_req(srp);
	sg_remove_request(sfp, srp);
	return err ? : err2 ? : count;
}

static ssize_t
sg_write(struct file *filp, const char __user *buf, size_t count, loff_t * ppos)
{
	int mxsize, cmd_size, k;
	int input_size, blocking;
	u8 opcode;
	struct sg_device *sdp;
	struct sg_fd *sfp;
	struct sg_request *srp;
	struct sg_io_hdr *hp;
	u8 cmnd[SG_MAX_CDB_SIZE];
	int retval;
	struct sg_header ohdr;

	retval = sg_check_file_access(filp, __func__);
	if (retval)
		return retval;

	sfp = (struct sg_fd *)filp->private_data;
	if (IS_ERR_OR_NULL(sfp)) {
		pr_warn("sg: %s: sfp is NULL or error\n", __func__);
		return IS_ERR(sfp) ? PTR_ERR(sfp) : -ENXIO;
	}
	sdp = sfp->parentdp;
	SG_LOG(3, sdp, "%s: write(3rd arg) count=%d\n", __func__, (int)count);
	if (IS_ERR_OR_NULL(sdp))
		return IS_ERR(sdp) ? PTR_ERR(sdp) : -ENXIO;
	if (atomic_read(&sdp->detaching))
		return -ENODEV;
	if (!((filp->f_flags & O_NONBLOCK) ||
	      scsi_block_when_processing_errors(sdp->device)))
		return -ENXIO;

	if (!access_ok(VERIFY_READ, buf, count))
		return -EFAULT;
	if (count < SZ_SG_HEADER)
		return -EIO;
	if (__copy_from_user(&ohdr, buf, SZ_SG_HEADER))
		return -EFAULT;
	blocking = !(filp->f_flags & O_NONBLOCK);
	if (ohdr.reply_len < 0)
		return sg_new_write(sfp, filp, buf, count,
				    blocking, 0, 0, NULL);
	if (count < (SZ_SG_HEADER + 6))
		return -EIO;	/* minimum scsi command length is 6 bytes */

	if (!(srp = sg_add_request(sfp))) {
		SG_LOG(1, sdp, "%s: queue full\n", __func__);
		return -EDOM;
	}
	buf += SZ_SG_HEADER;
	__get_user(opcode, buf);
	mutex_lock(&sfp->f_mutex);
	if (sfp->next_cmd_len > 0) {
		cmd_size = sfp->next_cmd_len;
		sfp->next_cmd_len = 0;	/* reset, only this write() effected */
	} else {
		cmd_size = COMMAND_SIZE(opcode);/* 'old; SCSI command group */
		if (opcode >= 0xc0 && ohdr.twelve_byte)
			cmd_size = 12;
	}
	mutex_unlock(&sfp->f_mutex);
	SG_LOG(4, sdp, "%s:   scsi opcode=0x%02x, cmd_size=%d\n", __func__,
	       (unsigned int)opcode, cmd_size);
	input_size = count - cmd_size;
	mxsize = (input_size > ohdr.reply_len) ? input_size : ohdr.reply_len;
	mxsize -= SZ_SG_HEADER;
	input_size -= SZ_SG_HEADER;
	if (input_size < 0) {
		sg_remove_request(sfp, srp);
		return -EIO; /* Insufficient bytes passed for this command. */
	}
	hp = &srp->header;
	hp->interface_id = '\0'; /* indicator of old interface tunnelled */
	hp->cmd_len = (u8)cmd_size;
	hp->iovec_count = 0;
	hp->mx_sb_len = 0;
	if (input_size > 0)
		hp->dxfer_direction = (ohdr.reply_len > SZ_SG_HEADER) ?
		    SG_DXFER_TO_FROM_DEV : SG_DXFER_TO_DEV;
	else
		hp->dxfer_direction = (mxsize > 0) ? SG_DXFER_FROM_DEV :
						     SG_DXFER_NONE;
	hp->dxfer_len = mxsize;
	if ((hp->dxfer_direction == SG_DXFER_TO_DEV) ||
	    (hp->dxfer_direction == SG_DXFER_TO_FROM_DEV))
		hp->dxferp = (char __user *)buf + cmd_size;
	else
		hp->dxferp = NULL;
	hp->sbp = NULL;
	hp->timeout = ohdr.reply_len;	/* structure abuse ... */
	hp->flags = input_size;	/* structure abuse ... */
	hp->pack_id = ohdr.pack_id;
	hp->usr_ptr = NULL;
	if (__copy_from_user(cmnd, buf, cmd_size))
		return -EFAULT;
	/*
	 * SG_DXFER_TO_FROM_DEV is functionally equivalent to SG_DXFER_FROM_DEV,
	 * but is is possible that the app intended SG_DXFER_TO_DEV, because there
	 * is a non-zero input_size, so emit a warning.
	 */
	if (hp->dxfer_direction == SG_DXFER_TO_FROM_DEV) {
		printk_ratelimited(KERN_WARNING
				   "sg_write: data in/out %d/%d bytes "
				   "for SCSI command 0x%x-- guessing "
				   "data in;\n   program %s not setting "
				   "count and/or reply_len properly\n",
				   ohdr.reply_len - (int)SZ_SG_HEADER,
				   input_size, (unsigned int)cmnd[0],
				   current->comm);
	}
	k = sg_common_write(sfp, srp, cmnd, sfp->timeout, blocking);
	return (k < 0) ? k : count;
}

static ssize_t
sg_new_write(struct sg_fd *sfp, struct file *file, const char __user *buf,
	     size_t count, int blocking, int read_only, int sg_io_owned,
	     struct sg_request **o_srp)
{
	int k;
	struct sg_request *srp;
	struct sg_io_hdr *hp;
	u8 cmnd[SG_MAX_CDB_SIZE];
	int timeout;
	unsigned long ul_timeout;

	if (count < SZ_SG_IO_HDR)
		return -EINVAL;
	if (!access_ok(VERIFY_READ, buf, count))
		return -EFAULT; /* protects following copy_from_user()s + get_user()s */

	sfp->cmd_q = true; /* when sg_io_hdr seen, set command queuing on */
	if (!(srp = sg_add_request(sfp))) {
		SG_LOG(1, sfp->parentdp, "%s: queue full\n", __func__);
		return -EDOM;
	}
	srp->sg_io_owned = sg_io_owned;
	hp = &srp->header;
	if (__copy_from_user(hp, buf, SZ_SG_IO_HDR)) {
		sg_remove_request(sfp, srp);
		return -EFAULT;
	}
	if (hp->interface_id != 'S') {
		sg_remove_request(sfp, srp);
		return -ENOSYS;
	}
	if (hp->flags & SG_FLAG_MMAP_IO) {
		if (hp->dxfer_len > sfp->reserve.dlen) {
			sg_remove_request(sfp, srp);
			return -ENOMEM;	/* MMAP_IO size must fit in reserve buffer */
		}
		if (hp->flags & SG_FLAG_DIRECT_IO) {
			sg_remove_request(sfp, srp);
			return -EINVAL;	/* either MMAP_IO or DIRECT_IO (not both) */
		}
		if (sfp->res_in_use) {
			sg_remove_request(sfp, srp);
			return -EBUSY;	/* reserve buffer already being used */
		}
	}
	ul_timeout = msecs_to_jiffies(srp->header.timeout);
	timeout = (ul_timeout < INT_MAX) ? ul_timeout : INT_MAX;
	if ((!hp->cmdp) || (hp->cmd_len < 6) || (hp->cmd_len > sizeof(cmnd))) {
		sg_remove_request(sfp, srp);
		return -EMSGSIZE;
	}
	if (!access_ok(VERIFY_READ, hp->cmdp, hp->cmd_len)) {
		sg_remove_request(sfp, srp);
		return -EFAULT;	/* protects following copy_from_user()s + get_user()s */
	}
	if (__copy_from_user(cmnd, hp->cmdp, hp->cmd_len)) {
		sg_remove_request(sfp, srp);
		return -EFAULT;
	}
	if (read_only && sg_allow_access(file, cmnd)) {
		sg_remove_request(sfp, srp);
		return -EPERM;
	}
	k = sg_common_write(sfp, srp, cmnd, timeout, blocking);
	if (k < 0)
		return k;
	if (o_srp)
		*o_srp = srp;
	return count;
}

static int
sg_common_write(struct sg_fd *sfp, struct sg_request *srp,
		u8 *cmnd, int timeout, int blocking)
{
	bool at_head;
	int k;
	struct sg_device *sdp = sfp->parentdp;
	struct sg_io_hdr *hp = &srp->header;

	srp->data.cmd_opcode = cmnd[0];	/* hold opcode of command */
	hp->status = 0;
	hp->masked_status = 0;
	hp->msg_status = 0;
	hp->info = 0;
	hp->host_status = 0;
	hp->driver_status = 0;
	hp->resid = 0;
	SG_LOG(4, sfp->parentdp, "%s:  scsi opcode=0x%02x, cmd_size=%d\n",
	       __func__, (int)cmnd[0], (int)hp->cmd_len);

	if (hp->dxfer_len >= SZ_256M)
		return -EINVAL;

	k = sg_start_req(srp, cmnd);
	if (k) {
		SG_LOG(1, sfp->parentdp, "%s: start_req err=%d\n", __func__,
		       k);
		sg_finish_rem_req(srp);
		sg_remove_request(sfp, srp);
		return k;	/* probably out of space --> ENOMEM */
	}
	if (atomic_read(&sdp->detaching)) {
		if (srp->bio) {
			scsi_req_free_cmd(scsi_req(srp->rq));
			blk_put_request(srp->rq);
			srp->rq = NULL;
		}

		sg_finish_rem_req(srp);
		sg_remove_request(sfp, srp);
		return -ENODEV;
	}

	hp->duration = jiffies_to_msecs(jiffies);
	/* at tail if v3 or later interface and tail flag set */
	at_head = !(hp->interface_id != '\0' &&
		    (SG_FLAG_Q_AT_TAIL & hp->flags));

	srp->rq->timeout = timeout;
	kref_get(&sfp->f_ref); /* sg_rq_end_io() does kref_put(). */
	blk_execute_rq_nowait(sdp->device->request_queue, sdp->disk,
			      srp->rq, (int)at_head, sg_rq_end_io);
	return 0;
}

static int
srp_done(struct sg_fd *sfp, struct sg_request *srp)
{
	unsigned long flags;
	int ret;

	read_lock_irqsave(&sfp->rq_list_lock, flags);
	ret = srp->done;
	read_unlock_irqrestore(&sfp->rq_list_lock, flags);
	return ret;
}

static int
max_sectors_bytes(struct request_queue *q)
{
	unsigned int max_sectors = queue_max_sectors(q);

	max_sectors = min_t(unsigned int, max_sectors, INT_MAX >> 9);

	return max_sectors << 9;
}

static void
sg_fill_request_table(struct sg_fd *sfp, struct sg_req_info *rinfo)
{
	struct sg_request *srp;
	int val;
	unsigned int ms;

	val = 0;
	list_for_each_entry(srp, &sfp->rq_list, entry) {
		if (val >= SG_MAX_QUEUE)
			break;
		rinfo[val].req_state = srp->done + 1;
		rinfo[val].problem =
			srp->header.masked_status &
			srp->header.host_status &
			srp->header.driver_status;
		if (srp->done)
			rinfo[val].duration =
				srp->header.duration;
		else {
			ms = jiffies_to_msecs(jiffies);
			rinfo[val].duration =
				(ms > srp->header.duration) ?
				(ms - srp->header.duration) : 0;
		}
		rinfo[val].orphan = srp->orphan;
		rinfo[val].sg_io_owned = srp->sg_io_owned;
		rinfo[val].pack_id = srp->header.pack_id;
		rinfo[val].usr_ptr = srp->header.usr_ptr;
		val++;
	}
}

#if 0	/* temporary to shorten big patch */
static long
sg_ioctl(struct file *filp, unsigned int cmd_in, unsigned long arg)
{
	void __user *p = (void __user *)arg;
	int __user *ip = p;
	int result, val, read_only;
	struct sg_device *sdp;
	struct sg_fd *sfp;
	struct sg_request *srp;
	unsigned long iflags;

	sfp = (struct sg_fd *)filp->private_data;
	if (!sfp)
		return -ENXIO;
	sdp = sfp->parentdp;
	if (!sdp)
		return -ENXIO;

	SG_LOG(3, sdp, "%s: cmd=0x%x\n", __func__, (int)cmd_in);
	read_only = (O_RDWR != (filp->f_flags & O_ACCMODE));

	switch (cmd_in) {
	case SG_IO:
		if (atomic_read(&sdp->detaching))
			return -ENODEV;
		if (!scsi_block_when_processing_errors(sdp->device))
			return -ENXIO;
		if (!access_ok(VERIFY_WRITE, p, SZ_SG_IO_HDR))
			return -EFAULT;
		result = sg_new_write(sfp, filp, p, SZ_SG_IO_HDR,
				 1, read_only, 1, &srp);
		if (result < 0)
			return result;
		result = wait_event_interruptible(sfp->read_wait,
			(srp_done(sfp, srp) || atomic_read(&sdp->detaching)));
		if (atomic_read(&sdp->detaching))
			return -ENODEV;
		write_lock_irq(&sfp->rq_list_lock);
		if (srp->done) {
			srp->done = 2;
			write_unlock_irq(&sfp->rq_list_lock);
			result = sg_new_read(sfp, p, SZ_SG_IO_HDR, srp);
			return (result < 0) ? result : 0;
		}
		srp->orphan = true;
		write_unlock_irq(&sfp->rq_list_lock);
		return result;	/* -ERESTARTSYS because signal hit process */
	case SG_SET_TIMEOUT:
		result = get_user(val, ip);
		if (result)
			return result;
		if (val < 0)
			return -EIO;
		if (val >= mult_frac((s64)INT_MAX, USER_HZ, HZ))
			val = min_t(s64, mult_frac((s64)INT_MAX, USER_HZ, HZ),
				    INT_MAX);
		sfp->timeout_user = val;
		sfp->timeout = mult_frac(val, HZ, USER_HZ);

		return 0;
	case SG_GET_TIMEOUT:	/* N.B. User receives timeout as return value */
				/* strange ..., for backward compatibility */
		return sfp->timeout_user;
	case SG_SET_FORCE_LOW_DMA:
		/*
		 * N.B. This ioctl never worked properly, but failed to
		 * return an error value. So returning '0' to keep compability
		 * with legacy applications.
		 */
		return 0;
	case SG_GET_LOW_DMA:
		return put_user((int)sdp->device->host->unchecked_isa_dma, ip);
	case SG_GET_SCSI_ID:
		if (!access_ok(VERIFY_WRITE, p, sizeof(struct sg_scsi_id)))
			return -EFAULT;
		else {
			struct sg_scsi_id __user *sg_idp = p;

			if (atomic_read(&sdp->detaching))
				return -ENODEV;
			__put_user((int)sdp->device->host->host_no,
				   &sg_idp->host_no);
			__put_user((int)sdp->device->channel,
				   &sg_idp->channel);
			__put_user((int)sdp->device->id, &sg_idp->scsi_id);
			__put_user((int)sdp->device->lun, &sg_idp->lun);
			__put_user((int)sdp->device->type, &sg_idp->scsi_type);
			__put_user((short)sdp->device->host->cmd_per_lun,
				   &sg_idp->h_cmd_per_lun);
			__put_user((short)sdp->device->queue_depth,
				   &sg_idp->d_queue_depth);
			__put_user(0, &sg_idp->unused[0]);
			__put_user(0, &sg_idp->unused[1]);
			return 0;
		}
	case SG_SET_FORCE_PACK_ID:
		result = get_user(val, ip);
		if (result)
			return result;
		sfp->force_packid = !!val;
		return 0;
	case SG_GET_PACK_ID:
		if (!access_ok(VERIFY_WRITE, ip, sizeof(int)))
			return -EFAULT;
		read_lock_irqsave(&sfp->rq_list_lock, iflags);
		list_for_each_entry(srp, &sfp->rq_list, entry) {
			if ((1 == srp->done) && (!srp->sg_io_owned)) {
				read_unlock_irqrestore(&sfp->rq_list_lock,
						       iflags);
				__put_user(srp->header.pack_id, ip);
				return 0;
			}
		}
		read_unlock_irqrestore(&sfp->rq_list_lock, iflags);
		__put_user(-1, ip);
		return 0;
	case SG_GET_NUM_WAITING:
		read_lock_irqsave(&sfp->rq_list_lock, iflags);
		val = 0;
		list_for_each_entry(srp, &sfp->rq_list, entry) {
			if ((1 == srp->done) && (!srp->sg_io_owned))
				++val;
		}
		read_unlock_irqrestore(&sfp->rq_list_lock, iflags);
		return put_user(val, ip);
	case SG_GET_SG_TABLESIZE:
		return put_user(sdp->sg_tablesize, ip);
	case SG_SET_RESERVED_SIZE:
		result = get_user(val, ip);
		if (result)
			return result;
                if (val < 0)
                        return -EINVAL;
		val = min_t(int, val,
			    max_sectors_bytes(sdp->device->request_queue));
		mutex_lock(&sfp->f_mutex);
		if (val != sfp->reserve.dlen) {
			if (sfp->mmap_called ||
			    sfp->res_in_use) {
				mutex_unlock(&sfp->f_mutex);
				return -EBUSY;
			}

			sg_remove_scat(sfp, &sfp->reserve);
			sg_build_reserve(sfp, val);
		}
		mutex_unlock(&sfp->f_mutex);
		return 0;
	case SG_GET_RESERVED_SIZE:
		val = min_t(int, sfp->reserve.dlen,
			    max_sectors_bytes(sdp->device->request_queue));
		return put_user(val, ip);
	case SG_SET_COMMAND_Q:
		result = get_user(val, ip);
		if (result)
			return result;
		sfp->cmd_q = !!val;
		return 0;
	case SG_GET_COMMAND_Q:
		return put_user((int)sfp->cmd_q, ip);
	case SG_SET_KEEP_ORPHAN:
		result = get_user(val, ip);
		if (result)
			return result;
		sfp->keep_orphan = !!val;
		return 0;
	case SG_GET_KEEP_ORPHAN:
		return put_user((int)sfp->keep_orphan, ip);
	case SG_NEXT_CMD_LEN:
		result = get_user(val, ip);
		if (result)
			return result;
		if (val > SG_MAX_CDB_SIZE)
			return -ENOMEM;
		sfp->next_cmd_len = (val > 0) ? val : 0;
		return 0;
	case SG_GET_VERSION_NUM:
		return put_user(sg_version_num, ip);
	case SG_GET_ACCESS_COUNT:
		/* faked - we don't have a real access count anymore */
		val = (sdp->device ? 1 : 0);
		return put_user(val, ip);
	case SG_GET_REQUEST_TABLE:
		if (!access_ok(VERIFY_WRITE, p, SZ_SG_REQ_INFO * SG_MAX_QUEUE))
			return -EFAULT;
		else {
			struct sg_req_info *rinfo;

			rinfo = kcalloc(SG_MAX_QUEUE, SZ_SG_REQ_INFO,
					GFP_KERNEL);
			if (!rinfo)
				return -ENOMEM;
			read_lock_irqsave(&sfp->rq_list_lock, iflags);
			sg_fill_request_table(sfp, rinfo);
			read_unlock_irqrestore(&sfp->rq_list_lock, iflags);
			result = __copy_to_user(p, rinfo,
						SZ_SG_REQ_INFO * SG_MAX_QUEUE);
			result = result ? -EFAULT : 0;
			kfree(rinfo);
			return result;
		}
	case SG_EMULATED_HOST:
		if (atomic_read(&sdp->detaching))
			return -ENODEV;
		return put_user(sdp->device->host->hostt->emulated, ip);
	case SCSI_IOCTL_SEND_COMMAND:
		if (atomic_read(&sdp->detaching))
			return -ENODEV;
		return sg_scsi_ioctl(sdp->device->request_queue, NULL, filp->f_mode, p);
	case SG_SET_DEBUG:
		result = get_user(val, ip);
		if (result)
			return result;
		sdp->sgdebug = (char) val;
		return 0;
	case BLKSECTGET:
		return put_user(max_sectors_bytes(sdp->device->request_queue),
				ip);
	case BLKTRACESETUP:
		return blk_trace_setup(sdp->device->request_queue,
				       sdp->disk->disk_name,
				       MKDEV(SCSI_GENERIC_MAJOR, sdp->index),
				       NULL, p);
	case BLKTRACESTART:
		return blk_trace_startstop(sdp->device->request_queue, 1);
	case BLKTRACESTOP:
		return blk_trace_startstop(sdp->device->request_queue, 0);
	case BLKTRACETEARDOWN:
		return blk_trace_remove(sdp->device->request_queue);
	case SCSI_IOCTL_GET_IDLUN:
	case SCSI_IOCTL_GET_BUS_NUMBER:
	case SCSI_IOCTL_PROBE_HOST:
	case SG_GET_TRANSFORM:
	case SG_SCSI_RESET:
		if (atomic_read(&sdp->detaching))
			return -ENODEV;
		break;
	default:
		if (read_only)
			return -EPERM;	/* don't know so take safe approach */
		break;
	}

	result = scsi_ioctl_block_when_processing_errors(sdp->device,
			cmd_in, filp->f_flags & O_NDELAY);
	if (result)
		return result;
	return scsi_ioctl(sdp->device, cmd_in, p);
}

#ifdef CONFIG_COMPAT
static long
sg_compat_ioctl(struct file *filp, unsigned int cmd_in, unsigned long arg)
{
	struct sg_device *sdp;
	struct sg_fd *sfp;
	struct scsi_device *sdev;

	sfp = (struct sg_fd *)filp->private_data;
	if (!sfp)
		return -ENXIO;
	sdp = sfp->parentdp;
	if (!sdp)
		return -ENXIO;

	sdev = sdp->device;
	if (sdev->host->hostt->compat_ioctl) { 
		int ret;

		ret = sdev->host->hostt->compat_ioctl(sdev, cmd_in, (void __user *)arg);

		return ret;
	}
	
	return -ENOIOCTLCMD;
}
#endif
#endif		/* temporary to shorten big patch */

static __poll_t
sg_poll(struct file *filp, poll_table * wait)
{
	__poll_t res = 0;
	struct sg_device *sdp;
	struct sg_fd *sfp;
	struct sg_request *srp;
	int count = 0;
	unsigned long iflags;

	sfp = filp->private_data;
	if (!sfp)
		return EPOLLERR;
	sdp = sfp->parentdp;
	if (!sdp)
		return EPOLLERR;
	poll_wait(filp, &sfp->read_wait, wait);
	read_lock_irqsave(&sfp->rq_list_lock, iflags);
	list_for_each_entry(srp, &sfp->rq_list, entry) {
		/* if any read waiting, flag it */
		if ((0 == res) && (1 == srp->done) && (!srp->sg_io_owned))
			res = EPOLLIN | EPOLLRDNORM;
		++count;
	}
	read_unlock_irqrestore(&sfp->rq_list_lock, iflags);

	if (atomic_read(&sdp->detaching))
		res |= EPOLLHUP;
	else if (!sfp->cmd_q) {
		if (0 == count)
			res |= EPOLLOUT | EPOLLWRNORM;
	} else if (count < SG_MAX_QUEUE)
		res |= EPOLLOUT | EPOLLWRNORM;
	SG_LOG(3, sdp, "%s: res=0x%x\n", __func__, (__force u32)res);
	return res;
}

static int
sg_fasync(int fd, struct file *filp, int mode)
{
	struct sg_device *sdp;
	struct sg_fd *sfp;

	sfp = (struct sg_fd *)filp->private_data;
	if (!sfp)
		return -ENXIO;
	sdp = sfp->parentdp;
	if (!sdp)
		return -ENXIO;
	SG_LOG(3, sdp, "%s: mode=%d\n", __func__, mode);

	return fasync_helper(fd, filp, mode, &sfp->async_qp);
}

static vm_fault_t
sg_vma_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct sg_fd *sfp;
	unsigned long offset, len, sa;
	struct sg_scatter_hold *rsv_schp;
	int k, length;

	if (!vma)
		return VM_FAULT_SIGBUS;
	sfp = (struct sg_fd *)vma->vm_private_data;
	if (!sfp)
		return VM_FAULT_SIGBUS;
	rsv_schp = &sfp->reserve;
	offset = vmf->pgoff << PAGE_SHIFT;
	if (offset >= rsv_schp->dlen)
		return VM_FAULT_SIGBUS;
	SG_LOG(3, sfp->parentdp, "%s: offset=%lu, scatg=%d\n", __func__,
	       offset, rsv_schp->num_sgat);
	sa = vma->vm_start;
	length = 1 << (PAGE_SHIFT + rsv_schp->page_order);
	for (k = 0; k < rsv_schp->num_sgat && sa < vma->vm_end; k++) {
		len = vma->vm_end - sa;
		len = (len < length) ? len : length;
		if (offset < len) {
			struct page *page = nth_page(rsv_schp->pages[k],
						     offset >> PAGE_SHIFT);
			get_page(page);	/* increment page count */
			vmf->page = page;
			return 0; /* success */
		}
		sa += len;
		offset -= len;
	}

	return VM_FAULT_SIGBUS;
}

static const struct vm_operations_struct sg_mmap_vm_ops = {
	.fault = sg_vma_fault,
};

static int
sg_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct sg_fd *sfp;
	unsigned long req_sz, len, sa;
	struct sg_scatter_hold *rsv_schp;
	int k, length;
	int ret = 0;

	if (!filp || !vma)
		return -ENXIO;
	sfp = (struct sg_fd *)filp->private_data;
	if (!sfp)
		return -ENXIO;
	req_sz = vma->vm_end - vma->vm_start;
	SG_LOG(3, sfp->parentdp, "%s starting, vm_start=%p, len=%d\n",
	       __func__, (void *)vma->vm_start, (int)req_sz);
	if (vma->vm_pgoff)
		return -EINVAL;	/* want no offset */
	rsv_schp = &sfp->reserve;
	mutex_lock(&sfp->f_mutex);
	if (req_sz > rsv_schp->dlen) {
		ret = -ENOMEM;	/* cannot map more than reserved buffer */
		goto out;
	}

	sa = vma->vm_start;
	length = 1 << (PAGE_SHIFT + rsv_schp->page_order);
	for (k = 0; k < rsv_schp->num_sgat && sa < vma->vm_end; k++) {
		len = vma->vm_end - sa;
		len = (len < length) ? len : length;
		sa += len;
	}

	sfp->mmap_called = true;
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
	vma->vm_private_data = sfp;
	vma->vm_ops = &sg_mmap_vm_ops;
out:
	mutex_unlock(&sfp->f_mutex);
	return ret;
}

/*
 * This user context function is needed to clean up a request that has been
 * interrupted (e.g. by control-C at keyboard). That leads to a request
 * being an 'orphan' and will be cleared here unless the 'keep_orphan' flag
 * has been set on the owning file descriptor. In that case the user is
 * expected to call read() or ioctl(SG_IORECEIVE) to receive the response
 * and free resources held by the interrupted request.
 */
static void
sg_rq_end_io_usercontext(struct work_struct *work)
{
	struct sg_request *srp = container_of(work, struct sg_request, ew.work);
	struct sg_fd *sfp = srp->parentfp;

	sg_finish_rem_req(srp);
	sg_remove_request(sfp, srp);
	kref_put(&sfp->f_ref, sg_remove_sfp);
}

/*
 * This function is a "bottom half" handler that is called by the mid-level
 * when a command is completed (or has failed). The function is a callback
 * registered in a blk_execute_rq_nowait() call at the end of the
 * sg_common_write() function. For synchronous usage with ioctl(SG_IO)
 * the function sg_sg_io() waits to be woken up by this callback.
 */
static void
sg_rq_end_io(struct request *rq, blk_status_t status)
{
	struct sg_request *srp = rq->end_io_data;
	struct scsi_request *req = scsi_req(rq);
	struct sg_device *sdp;
	struct sg_fd *sfp;
	unsigned long iflags;
	unsigned int ms;
	u8 *sense;
	int result, resid, done = 1;

	if (WARN_ON(srp->done != 0))
		return;

	sfp = srp->parentfp;
	if (WARN_ON(sfp == NULL))
		return;

	sdp = sfp->parentdp;
	if (unlikely(atomic_read(&sdp->detaching)))
		pr_info("%s: device detaching\n", __func__);

	sense = req->sense;
	result = req->result;
	resid = req->resid_len;

	SG_LOG(4, sdp, "%s: pack_id=%d, res=0x%x\n", __func__,
	       srp->header.pack_id, result);
	srp->header.resid = resid;
	ms = jiffies_to_msecs(jiffies);
	srp->header.duration = (ms > srp->header.duration) ?
				(ms - srp->header.duration) : 0;
	if (0 != result) {
		struct scsi_sense_hdr sshdr;

		srp->header.status = 0xff & result;
		srp->header.masked_status = status_byte(result);
		srp->header.msg_status = msg_byte(result);
		srp->header.host_status = host_byte(result);
		srp->header.driver_status = driver_byte(result);
		if ((sdp->sgdebug > 0) &&
		    ((CHECK_CONDITION == srp->header.masked_status) ||
		     (COMMAND_TERMINATED == srp->header.masked_status)))
			__scsi_print_sense(sdp->device, __func__, sense,
					   SCSI_SENSE_BUFFERSIZE);

		/* Following if statement is a patch supplied by Eric Youngdale */
		if (driver_byte(result) != 0
		    && scsi_normalize_sense(sense, SCSI_SENSE_BUFFERSIZE, &sshdr)
		    && !scsi_sense_is_deferred(&sshdr)
		    && sshdr.sense_key == UNIT_ATTENTION
		    && sdp->device->removable) {
			/* Detected possible disc change. Set the bit - this */
			/* may be used if there are filesystems using this device */
			sdp->device->changed = 1;
		}
	}

	if (req->sense_len)
		memcpy(srp->sense_b, req->sense, SCSI_SENSE_BUFFERSIZE);

	/* Rely on write phase to clean out srp status values, so no "else" */

	/*
	 * Free the request as soon as it is complete so that its resources
	 * can be reused without waiting for userspace to read() the
	 * result.  But keep the associated bio (if any) around until
	 * blk_rq_unmap_user() can be called from user context.
	 */
	srp->rq = NULL;
	scsi_req_free_cmd(scsi_req(rq));
	__blk_put_request(rq->q, rq);

	write_lock_irqsave(&sfp->rq_list_lock, iflags);
	if (unlikely(srp->orphan)) {
		if (sfp->keep_orphan)
			srp->sg_io_owned = 0;
		else
			done = 0;
	}
	srp->done = done;
	write_unlock_irqrestore(&sfp->rq_list_lock, iflags);

	if (likely(done)) {
		/* Now wake up any sg_read() that is waiting for this
		 * packet.
		 */
		wake_up_interruptible(&sfp->read_wait);
		kill_fasync(&sfp->async_qp, SIGPOLL, POLL_IN);
		kref_put(&sfp->f_ref, sg_remove_sfp);
	} else {
		INIT_WORK(&srp->ew.work, sg_rq_end_io_usercontext);
		schedule_work(&srp->ew.work);
	}
}

static const struct file_operations sg_fops = {
	.owner = THIS_MODULE,
	.read = sg_read,
	.write = sg_write,
	.poll = sg_poll,
#if 0	/* temporary to shorten big patch */
	.unlocked_ioctl = sg_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = sg_compat_ioctl,
#endif
#endif		/* temporary to shorten big patch */
	.open = sg_open,
	.mmap = sg_mmap,
	.release = sg_release,
	.fasync = sg_fasync,
	.llseek = no_llseek,
};

static struct class *sg_sysfs_class;

static int sg_sysfs_valid = 0;

static struct sg_device *
sg_alloc(struct gendisk *disk, struct scsi_device *scsidp)
{
	struct request_queue *q = scsidp->request_queue;
	struct sg_device *sdp;
	unsigned long iflags;
	int error;
	u32 k;

	sdp = kzalloc(sizeof(struct sg_device), GFP_KERNEL);
	if (!sdp)
		return ERR_PTR(-ENOMEM);

	idr_preload(GFP_KERNEL);
	write_lock_irqsave(&sg_index_lock, iflags);

	error = idr_alloc(&sg_index_idr, sdp, 0, SG_MAX_DEVS, GFP_NOWAIT);
	if (error < 0) {
		if (error == -ENOSPC) {
			sdev_printk(KERN_WARNING, scsidp,
				    "Unable to attach sg device type=%d, minor number exceeds %d\n",
				    scsidp->type, SG_MAX_DEVS - 1);
			error = -ENODEV;
		} else {
			sdev_printk(KERN_WARNING, scsidp,
				"%s: idr allocation sg_device failure: %d\n",
				    __func__, error);
		}
		goto out_unlock;
	}
	k = error;

	SCSI_LOG_TIMEOUT(3, sdev_printk(KERN_INFO, scsidp,
					"sg_alloc: dev=%d \n", k));
	sprintf(disk->disk_name, "sg%d", k);
	disk->first_minor = k;
	sdp->disk = disk;
	sdp->device = scsidp;
	mutex_init(&sdp->open_rel_lock);
	INIT_LIST_HEAD(&sdp->sfds);
	init_waitqueue_head(&sdp->open_wait);
	atomic_set(&sdp->detaching, 0);
	rwlock_init(&sdp->sfd_lock);
	sdp->sg_tablesize = queue_max_segments(q);
	sdp->index = k;
	kref_init(&sdp->d_ref);
	error = 0;

out_unlock:
	write_unlock_irqrestore(&sg_index_lock, iflags);
	idr_preload_end();

	if (error) {
		kfree(sdp);
		return ERR_PTR(error);
	}
	return sdp;
}

static int
sg_add_device(struct device *cl_dev, struct class_interface *cl_intf)
{
	struct scsi_device *scsidp = to_scsi_device(cl_dev->parent);
	struct gendisk *disk;
	struct sg_device *sdp = NULL;
	struct cdev * cdev = NULL;
	int error;
	unsigned long iflags;

	disk = alloc_disk(1);
	if (!disk) {
		pr_warn("%s: alloc_disk failed\n", __func__);
		return -ENOMEM;
	}
	disk->major = SCSI_GENERIC_MAJOR;

	error = -ENOMEM;
	cdev = cdev_alloc();
	if (!cdev) {
		pr_warn("%s: cdev_alloc failed\n", __func__);
		goto out;
	}
	cdev->owner = THIS_MODULE;
	cdev->ops = &sg_fops;

	sdp = sg_alloc(disk, scsidp);
	if (IS_ERR(sdp)) {
		pr_warn("%s: sg_alloc failed\n", __func__);
		error = PTR_ERR(sdp);
		goto out;
	}

	error = cdev_add(cdev, MKDEV(SCSI_GENERIC_MAJOR, sdp->index), 1);
	if (error)
		goto cdev_add_err;

	sdp->cdev = cdev;
	if (sg_sysfs_valid) {
		struct device *sg_class_member;

		sg_class_member = device_create(sg_sysfs_class, cl_dev->parent,
						MKDEV(SCSI_GENERIC_MAJOR,
						      sdp->index),
						sdp, "%s", disk->disk_name);
		if (IS_ERR(sg_class_member)) {
			pr_err("%s: device_create failed\n", __func__);
			error = PTR_ERR(sg_class_member);
			goto cdev_add_err;
		}
		error = sysfs_create_link(&scsidp->sdev_gendev.kobj,
					  &sg_class_member->kobj, "generic");
		if (error)
			pr_err("%s: unable to make symlink 'generic' back "
			       "to sg%d\n", __func__, sdp->index);
	} else
		pr_warn("%s: sg_sys Invalid\n", __func__);

	sdev_printk(KERN_NOTICE, scsidp, "Attached scsi generic sg%d "
		    "type %d\n", sdp->index, scsidp->type);

	dev_set_drvdata(cl_dev, sdp);

	return 0;

cdev_add_err:
	write_lock_irqsave(&sg_index_lock, iflags);
	idr_remove(&sg_index_idr, sdp->index);
	write_unlock_irqrestore(&sg_index_lock, iflags);
	kfree(sdp);

out:
	put_disk(disk);
	if (cdev)
		cdev_del(cdev);
	return error;
}

static void
sg_device_destroy(struct kref *kref)
{
	struct sg_device *sdp = container_of(kref, struct sg_device, d_ref);
	unsigned long flags;

	/* CAUTION!  Note that the device can still be found via idr_find()
	 * even though the refcount is 0.  Therefore, do idr_remove() BEFORE
	 * any other cleanup.
	 */

	write_lock_irqsave(&sg_index_lock, flags);
	idr_remove(&sg_index_idr, sdp->index);
	write_unlock_irqrestore(&sg_index_lock, flags);

	SG_LOG(3, sdp, "%s\n", __func__);

	put_disk(sdp->disk);
	kfree(sdp);
}

static void
sg_remove_device(struct device *cl_dev, struct class_interface *cl_intf)
{
	struct scsi_device *scsidp = to_scsi_device(cl_dev->parent);
	struct sg_device *sdp = dev_get_drvdata(cl_dev);
	unsigned long iflags;
	struct sg_fd *sfp;
	int val;

	if (!sdp)
		return;
	/* want sdp->detaching non-zero as soon as possible */
	val = atomic_inc_return(&sdp->detaching);
	if (val > 1)
		return; /* only want to do following once per device */

	SG_LOG(3, sdp, "%s\n", __func__);

	read_lock_irqsave(&sdp->sfd_lock, iflags);
	list_for_each_entry(sfp, &sdp->sfds, sfd_siblings) {
		wake_up_interruptible_all(&sfp->read_wait);
		kill_fasync(&sfp->async_qp, SIGPOLL, POLL_HUP);
	}
	wake_up_interruptible_all(&sdp->open_wait);
	read_unlock_irqrestore(&sdp->sfd_lock, iflags);

	sysfs_remove_link(&scsidp->sdev_gendev.kobj, "generic");
	device_destroy(sg_sysfs_class, MKDEV(SCSI_GENERIC_MAJOR, sdp->index));
	cdev_del(sdp->cdev);
	sdp->cdev = NULL;

	kref_put(&sdp->d_ref, sg_device_destroy);
}

module_param_named(scatter_elem_sz, scatter_elem_sz, int, S_IRUGO | S_IWUSR);
module_param_named(def_reserved_size, def_reserved_size, int,
		   S_IRUGO | S_IWUSR);
module_param_named(allow_dio, sg_allow_dio, int, S_IRUGO | S_IWUSR);

MODULE_AUTHOR("Douglas Gilbert");
MODULE_DESCRIPTION("SCSI generic (sg) driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(SG_VERSION_STR);
MODULE_ALIAS_CHARDEV_MAJOR(SCSI_GENERIC_MAJOR);

MODULE_PARM_DESC(scatter_elem_sz, "scatter gather element "
                "size (default: max(SG_SCATTER_SZ, PAGE_SIZE))");
MODULE_PARM_DESC(def_reserved_size, "size of buffer reserved for each fd");
MODULE_PARM_DESC(allow_dio, "allow direct I/O (default: 0 (disallow))");

static int __init
init_sg(void)
{
	int rc;

	if (scatter_elem_sz < PAGE_SIZE) {
		scatter_elem_sz = PAGE_SIZE;
		scatter_elem_sz_prev = scatter_elem_sz;
	}
	if (def_reserved_size >= 0)
		sg_big_buff = def_reserved_size;
	else
		def_reserved_size = sg_big_buff;

	rc = register_chrdev_region(MKDEV(SCSI_GENERIC_MAJOR, 0), 
				    SG_MAX_DEVS, "sg");
	if (rc)
		return rc;
        sg_sysfs_class = class_create(THIS_MODULE, "scsi_generic");
        if ( IS_ERR(sg_sysfs_class) ) {
		rc = PTR_ERR(sg_sysfs_class);
		goto err_out;
        }
	sg_sysfs_valid = 1;
	rc = scsi_register_interface(&sg_interface);
	if (0 == rc) {
#ifdef CONFIG_SCSI_PROC_FS
		sg_proc_init();
#endif				/* CONFIG_SCSI_PROC_FS */
		return 0;
	}
	class_destroy(sg_sysfs_class);
err_out:
	unregister_chrdev_region(MKDEV(SCSI_GENERIC_MAJOR, 0), SG_MAX_DEVS);
	return rc;
}

static void __exit
exit_sg(void)
{
#ifdef CONFIG_SCSI_PROC_FS
	remove_proc_subtree("scsi/sg", NULL);
#endif				/* CONFIG_SCSI_PROC_FS */
	scsi_unregister_interface(&sg_interface);
	class_destroy(sg_sysfs_class);
	sg_sysfs_valid = 0;
	unregister_chrdev_region(MKDEV(SCSI_GENERIC_MAJOR, 0),
				 SG_MAX_DEVS);
	idr_destroy(&sg_index_idr);
}

/* Returns 0 if okay, otherwise negated errno value */
static int
sg_start_req(struct sg_request *srp, u8 *cmd)
{
	int res;
	struct request *rq;
	struct scsi_request *req;
	struct sg_fd *sfp = srp->parentfp;
	struct sg_io_hdr *hp = &srp->header;
	int dxfer_len = (int)hp->dxfer_len;
	int dxfer_dir = hp->dxfer_direction;
	unsigned int iov_count = hp->iovec_count;
	struct sg_scatter_hold *req_schp = &srp->data;
	struct sg_scatter_hold *rsv_schp = &sfp->reserve;
	struct request_queue *q = sfp->parentdp->device->request_queue;
	struct rq_map_data *md, map_data;
	int rw = hp->dxfer_direction == SG_DXFER_TO_DEV ? WRITE : READ;
	u8 *long_cmdp = NULL;

	SG_LOG(4, sfp->parentdp, "%s: dxfer_len=%d\n", __func__, dxfer_len);

	if (hp->cmd_len > BLK_MAX_CDB) {
		long_cmdp = kzalloc(hp->cmd_len, GFP_KERNEL);
		if (!long_cmdp)
			return -ENOMEM;
	}

	/*
	 * NOTE
	 *
	 * With scsi-mq enabled, there are a fixed number of preallocated
	 * requests equal in number to shost->can_queue.  If all of the
	 * preallocated requests are already in use, then blk_get_request()
	 * will sleep until an active command completes, freeing up a request.
	 * Although waiting in an asynchronous interface is less than ideal, we
	 * do not want to use BLK_MQ_REQ_NOWAIT here because userspace might
	 * not expect an EWOULDBLOCK from this condition.
	 */
	rq = blk_get_request(q, hp->dxfer_direction == SG_DXFER_TO_DEV ?
			REQ_OP_SCSI_OUT : REQ_OP_SCSI_IN, 0);
	if (IS_ERR(rq)) {
		kfree(long_cmdp);
		return PTR_ERR(rq);
	}
	req = scsi_req(rq);

	if (hp->cmd_len > BLK_MAX_CDB)
		req->cmd = long_cmdp;
	memcpy(req->cmd, cmd, hp->cmd_len);
	req->cmd_len = hp->cmd_len;

	srp->rq = rq;
	rq->end_io_data = srp;
	req->retries = SG_DEFAULT_RETRIES;

	if ((dxfer_len <= 0) || (dxfer_dir == SG_DXFER_NONE))
		return 0;

	if (sg_allow_dio && hp->flags & SG_FLAG_DIRECT_IO &&
	    dxfer_dir != SG_DXFER_UNKNOWN && !iov_count &&
	    !sfp->parentdp->device->host->unchecked_isa_dma &&
	    blk_rq_aligned(q, (unsigned long)hp->dxferp, dxfer_len))
		md = NULL;
	else
		md = &map_data;

	if (md) {
		mutex_lock(&sfp->f_mutex);
		if (dxfer_len <= rsv_schp->dlen &&
		    !sfp->res_in_use) {
			sfp->res_in_use = true;
			sg_link_reserve(sfp, srp, dxfer_len);
		} else if (hp->flags & SG_FLAG_MMAP_IO) {
			res = -EBUSY; /* sfp->res_in_use == true */
			if (dxfer_len > rsv_schp->dlen)
				res = -ENOMEM;
			mutex_unlock(&sfp->f_mutex);
			return res;
		} else {
			res = sg_build_indirect(req_schp, sfp, dxfer_len);
			if (res) {
				mutex_unlock(&sfp->f_mutex);
				return res;
			}
		}
		mutex_unlock(&sfp->f_mutex);

		md->pages = req_schp->pages;
		md->page_order = req_schp->page_order;
		md->nr_entries = req_schp->num_sgat;
		md->offset = 0;
		md->null_mapped = hp->dxferp ? 0 : 1;
		if (dxfer_dir == SG_DXFER_TO_FROM_DEV)
			md->from_user = 1;
		else
			md->from_user = 0;
	}

	if (iov_count) {
		struct iovec *iov = NULL;
		struct iov_iter i;

		res = import_iovec(rw, hp->dxferp, iov_count, 0, &iov, &i);
		if (res < 0)
			return res;

		iov_iter_truncate(&i, hp->dxfer_len);
		if (!iov_iter_count(&i)) {
			kfree(iov);
			return -EINVAL;
		}

		res = blk_rq_map_user_iov(q, rq, md, &i, GFP_ATOMIC);
		kfree(iov);
	} else
		res = blk_rq_map_user(q, rq, md, hp->dxferp,
				      hp->dxfer_len, GFP_ATOMIC);

	if (!res) {
		srp->bio = rq->bio;

		if (!md) {
			req_schp->dio_in_use = true;
			hp->info |= SG_INFO_DIRECT_IO;
		}
	}
	return res;
}

static int
sg_finish_rem_req(struct sg_request *srp)
{
	int ret = 0;

	struct sg_fd *sfp = srp->parentfp;
	struct sg_scatter_hold *req_schp = &srp->data;

	SG_LOG(4, sfp->parentdp, "%s: res_used=%d\n", __func__,
	       (int)srp->res_used);
	if (srp->bio)
		ret = blk_rq_unmap_user(srp->bio);

	if (srp->rq) {
		scsi_req_free_cmd(scsi_req(srp->rq));
		blk_put_request(srp->rq);
	}

	if (srp->res_used)
		sg_unlink_reserve(sfp, srp);
	else
		sg_remove_scat(sfp, req_schp);

	return ret;
}

static int
sg_build_sgat(struct sg_scatter_hold *schp, const struct sg_fd *sfp,
	      int tablesize)
{
	int sg_bufflen = tablesize * sizeof(struct page *);
	gfp_t gfp_flags = GFP_ATOMIC | __GFP_NOWARN;

	schp->pages = kzalloc(sg_bufflen, gfp_flags);
	if (!schp->pages)
		return -ENOMEM;
	return tablesize;	/* number of scat_gath elements allocated */
}

static int
sg_build_indirect(struct sg_scatter_hold *schp, struct sg_fd *sfp,
		  int buff_size)
{
	int ret_sz = 0, i, k, rem_sz, num, mx_sc_elems;
	int sg_tablesize = sfp->parentdp->sg_tablesize;
	int blk_size = buff_size, order;
	gfp_t gfp_mask = GFP_ATOMIC | __GFP_COMP | __GFP_NOWARN | __GFP_ZERO;
	struct sg_device *sdp = sfp->parentdp;

	if (blk_size < 0)
		return -EFAULT;
	if (0 == blk_size)
		++blk_size;	/* don't know why */
	/* round request up to next highest SG_SECTOR_SZ byte boundary */
	blk_size = ALIGN(blk_size, SG_SECTOR_SZ);
	SG_LOG(4, sfp->parentdp, "%s: buff_size=%d, blk_size=%d\n",
	       __func__, buff_size, blk_size);

	/* N.B. ret_sz carried into this block ... */
	mx_sc_elems = sg_build_sgat(schp, sfp, sg_tablesize);
	if (mx_sc_elems < 0)
		return mx_sc_elems;	/* most likely -ENOMEM */

	num = scatter_elem_sz;
	if (unlikely(num != scatter_elem_sz_prev)) {
		if (num < PAGE_SIZE) {
			scatter_elem_sz = PAGE_SIZE;
			scatter_elem_sz_prev = PAGE_SIZE;
		} else
			scatter_elem_sz_prev = num;
	}

	if (sdp->device->host->unchecked_isa_dma)
		gfp_mask |= GFP_DMA;

	order = get_order(num);
retry:
	ret_sz = 1 << (PAGE_SHIFT + order);

	for (k = 0, rem_sz = blk_size; rem_sz > 0 && k < mx_sc_elems;
	     k++, rem_sz -= ret_sz) {

		num = (rem_sz > scatter_elem_sz_prev) ?
			scatter_elem_sz_prev : rem_sz;

		schp->pages[k] = alloc_pages(gfp_mask, order);
		if (!schp->pages[k])
			goto out;

		if (num == scatter_elem_sz_prev) {
			if (unlikely(ret_sz > scatter_elem_sz_prev)) {
				scatter_elem_sz = ret_sz;
				scatter_elem_sz_prev = ret_sz;
			}
		}

		SG_LOG(5, sfp->parentdp, "%s: k=%d, num=%d, ret_sz=%d\n",
		       __func__, k, num, ret_sz);
	}		/* end of for loop */

	schp->page_order = order;
	schp->num_sgat = k;
	SG_LOG(5, sfp->parentdp, "%s: num_sgat=%d, rem_sz=%d\n", __func__, k,
	       rem_sz);

	schp->dlen = blk_size;
	if (rem_sz > 0)	/* must have failed */
		return -ENOMEM;
	return 0;
out:
	for (i = 0; i < k; i++)
		__free_pages(schp->pages[i], order);

	if (--order >= 0)
		goto retry;

	return -ENOMEM;
}

static void
sg_remove_scat(struct sg_fd *sfp, struct sg_scatter_hold *schp)
{
	SG_LOG(4, sfp->parentdp, "%s: num_sgat=%d\n", __func__,
	       schp->num_sgat);
	if (schp->pages) {
		if (!schp->dio_in_use) {
			int k;

			for (k = 0; k < schp->num_sgat && schp->pages[k]; k++) {
				SG_LOG(5, sfp->parentdp, "%s: k=%d, pg=0x%p\n",
				       __func__, k, schp->pages[k]);
				__free_pages(schp->pages[k], schp->page_order);
			}

			kfree(schp->pages);
		}
	}
	memset(schp, 0, sizeof(*schp));
}

static int
sg_read_oxfer(struct sg_request *srp, char __user *outp, int num_read_xfer)
{
	struct sg_scatter_hold *schp = &srp->data;
	int k, num;

	SG_LOG(4, srp->parentfp->parentdp, "%s: num_read_xfer=%d\n", __func__,
	       num_read_xfer);
	if ((!outp) || (num_read_xfer <= 0))
		return 0;

	num = 1 << (PAGE_SHIFT + schp->page_order);
	for (k = 0; k < schp->num_sgat && schp->pages[k]; k++) {
		if (num > num_read_xfer) {
			if (__copy_to_user(outp, page_address(schp->pages[k]),
					   num_read_xfer))
				return -EFAULT;
			break;
		} else {
			if (__copy_to_user(outp, page_address(schp->pages[k]),
					   num))
				return -EFAULT;
			num_read_xfer -= num;
			if (num_read_xfer <= 0)
				break;
			outp += num;
		}
	}

	return 0;
}

static void
sg_build_reserve(struct sg_fd *sfp, int req_size)
{
	struct sg_scatter_hold *schp = &sfp->reserve;

	SG_LOG(4, sfp->parentdp, "%s: req_size=%d\n", __func__, req_size);
	do {
		if (req_size < PAGE_SIZE)
			req_size = PAGE_SIZE;
		if (0 == sg_build_indirect(schp, sfp, req_size))
			return;
		else
			sg_remove_scat(sfp, schp);
		req_size >>= 1;	/* divide by 2 */
	} while (req_size > (PAGE_SIZE / 2));
}

static void
sg_link_reserve(struct sg_fd *sfp, struct sg_request *srp, int size)
{
	struct sg_scatter_hold *req_schp = &srp->data;
	struct sg_scatter_hold *rsv_schp = &sfp->reserve;
	int k, num, rem;

	srp->res_used = true;
	SG_LOG(4, sfp->parentdp, "%s: size=%d\n", __func__, size);
	rem = size;

	num = 1 << (PAGE_SHIFT + rsv_schp->page_order);
	for (k = 0; k < rsv_schp->num_sgat; k++) {
		if (rem <= num) {
			req_schp->num_sgat = k + 1;
			req_schp->pages = rsv_schp->pages;

			req_schp->dlen = size;
			req_schp->page_order = rsv_schp->page_order;
			break;
		} else
			rem -= num;
	}

	if (k >= rsv_schp->num_sgat)
		SG_LOG(1, sfp->parentdp, "%s: BAD size\n", __func__);
}

static void
sg_unlink_reserve(struct sg_fd *sfp, struct sg_request *srp)
{
	struct sg_scatter_hold *req_schp = &srp->data;

	SG_LOG(4, srp->parentfp->parentdp, "%s: req->num_sgat=%d\n", __func__,
	       (int)req_schp->num_sgat);
	req_schp->num_sgat = 0;
	req_schp->dlen = 0;
	req_schp->pages = NULL;
	req_schp->page_order = 0;
	srp->res_used = false;
	/* Called without mutex lock to avoid deadlock */
	sfp->res_in_use = false;
}

static struct sg_request *
sg_get_rq_pack_id(struct sg_fd *sfp, int pack_id)
{
	struct sg_request *resp;
	unsigned long iflags;

	write_lock_irqsave(&sfp->rq_list_lock, iflags);
	list_for_each_entry(resp, &sfp->rq_list, entry) {
		/* look for requests that are ready + not SG_IO owned */
		if ((1 == resp->done) && (!resp->sg_io_owned) &&
		    ((-1 == pack_id) || (resp->header.pack_id == pack_id))) {
			resp->done = 2;	/* guard against other readers */
			write_unlock_irqrestore(&sfp->rq_list_lock, iflags);
			return resp;
		}
	}
	write_unlock_irqrestore(&sfp->rq_list_lock, iflags);
	return NULL;
}

/*
 * Adds an active request (soon to carry a SCSI command) to the current file
 * descriptor by creating a new one or re-using a request from the free
 * list (fl).  Returns a valid pointer if successful. On failure returns a
 * negated errno value twisted by ERR_PTR() macro.
 */
static struct sg_request *
sg_add_request(struct sg_fd *sfp)
{
	int k;
	unsigned long iflags;
	struct sg_request *rp = sfp->req_arr;

	write_lock_irqsave(&sfp->rq_list_lock, iflags);
	if (!list_empty(&sfp->rq_list)) {
		if (!sfp->cmd_q)
			goto out_unlock;

		for (k = 0; k < SG_MAX_QUEUE; ++k, ++rp) {
			if (!rp->parentfp)
				break;
		}
		if (k >= SG_MAX_QUEUE)
			goto out_unlock;
	}
	memset(rp, 0, sizeof(*rp));
	rp->parentfp = sfp;
	rp->header.duration = jiffies_to_msecs(jiffies);
	list_add_tail(&rp->entry, &sfp->rq_list);
	write_unlock_irqrestore(&sfp->rq_list_lock, iflags);
	return rp;
out_unlock:
	write_unlock_irqrestore(&sfp->rq_list_lock, iflags);
	return NULL;
}

/*
 * Moves a completed sg_request object to the free list and set it to
 * SG_RQ_INACTIVE which makes it available for re-use. Requests with
 * no data associated are appended to the tail of the free list while
 * other requests are prepended to the head of the free list. If the
 * data length exceeds rem_sgat_thresh then the data (or sgat) is
 * cleared and the request is appended to the tail of the free list.
 */
static int
sg_remove_request(struct sg_fd *sfp, struct sg_request *srp)
{
	unsigned long iflags;
	int res = 0;

	if (!sfp || !srp || list_empty(&sfp->rq_list))
		return res;
	write_lock_irqsave(&sfp->rq_list_lock, iflags);
	if (!list_empty(&srp->entry)) {
		list_del(&srp->entry);
		srp->parentfp = NULL;
		res = 1;
	}
	write_unlock_irqrestore(&sfp->rq_list_lock, iflags);
	return res;
}

static struct sg_fd *
sg_add_sfp(struct sg_device *sdp)
{
	struct sg_fd *sfp;
	unsigned long iflags;
	int bufflen;

	sfp = kzalloc(sizeof(*sfp), GFP_ATOMIC | __GFP_NOWARN);
	if (!sfp)
		return ERR_PTR(-ENOMEM);

	init_waitqueue_head(&sfp->read_wait);
	rwlock_init(&sfp->rq_list_lock);
	INIT_LIST_HEAD(&sfp->rq_list);
	kref_init(&sfp->f_ref);
	mutex_init(&sfp->f_mutex);
	sfp->timeout = SG_DEFAULT_TIMEOUT;
	sfp->timeout_user = SG_DEFAULT_TIMEOUT_USER;
	sfp->force_packid = !!SG_DEF_FORCE_PACK_ID;
	sfp->cmd_q = !!SG_DEF_COMMAND_Q;
	sfp->keep_orphan = !!SG_DEF_KEEP_ORPHAN;
	sfp->parentdp = sdp;
	write_lock_irqsave(&sdp->sfd_lock, iflags);
	if (atomic_read(&sdp->detaching)) {
		write_unlock_irqrestore(&sdp->sfd_lock, iflags);
		kfree(sfp);
		return ERR_PTR(-ENODEV);
	}
	list_add_tail(&sfp->sfd_siblings, &sdp->sfds);
	write_unlock_irqrestore(&sdp->sfd_lock, iflags);
	SG_LOG(3, sdp, "%s: sfp=0x%p\n", __func__, sfp);
	if (unlikely(sg_big_buff != def_reserved_size))
		sg_big_buff = def_reserved_size;

	bufflen = min_t(int, sg_big_buff,
			max_sectors_bytes(sdp->device->request_queue));
	sg_build_reserve(sfp, bufflen);
	SG_LOG(3, sdp, "%s: dlen=%d, num_sgat=%d\n", __func__,
	       sfp->reserve.dlen, sfp->reserve.num_sgat);

	kref_get(&sdp->d_ref);
	__module_get(THIS_MODULE);
	return sfp;
}

/*
 * All requests associated with this file descriptor should be completed or
 * cancelled when this function is called (due to sfp->f_ref). Also the
 * file descriptor itself has not been accessible since it was list_del()-ed
 * by the preceding sg_remove_sfp() call. So no locking is required. sdp
 * should never be NULL but to make debugging more robust, this function
 * will not blow up in that case.
 */
static void
sg_remove_sfp_usercontext(struct work_struct *work)
{
	struct sg_fd *sfp = container_of(work, struct sg_fd, ew.work);
	struct sg_device *sdp = sfp->parentdp;
	struct sg_request *srp;
	unsigned long iflags;

	/* Cleanup any responses which were never read(). */
	write_lock_irqsave(&sfp->rq_list_lock, iflags);
	while (!list_empty(&sfp->rq_list)) {
		srp = list_first_entry(&sfp->rq_list, struct sg_request,
				       entry);
		sg_finish_rem_req(srp);
		list_del(&srp->entry);
		srp->parentfp = NULL;
	}
	write_unlock_irqrestore(&sfp->rq_list_lock, iflags);

	if (sfp->reserve.dlen > 0) {
		SG_LOG(6, sdp, "%s:    dlen=%d, num_sgat=%d\n", __func__,
		       (int)sfp->reserve.dlen,
		       (int)sfp->reserve.num_sgat);
		sg_remove_scat(sfp, &sfp->reserve);
	}

	SG_LOG(6, sdp, "%s: sfp=0x%p\n", __func__, sfp);
	kfree(sfp);

	scsi_device_put(sdp->device);
	kref_put(&sdp->d_ref, sg_device_destroy);
	module_put(THIS_MODULE);
}

static void
sg_remove_sfp(struct kref *kref)
{
	struct sg_fd *sfp = container_of(kref, struct sg_fd, f_ref);
	struct sg_device *sdp = sfp->parentdp;
	unsigned long iflags;

	write_lock_irqsave(&sdp->sfd_lock, iflags);
	list_del(&sfp->sfd_siblings);
	write_unlock_irqrestore(&sdp->sfd_lock, iflags);

	INIT_WORK(&sfp->ew.work, sg_remove_sfp_usercontext);
	schedule_work(&sfp->ew.work);
}

#ifdef CONFIG_SCSI_PROC_FS
static int
sg_idr_max_id(int id, void *p, void *data)
{
	int *k = data;

	if (*k < id)
		*k = id;

	return 0;
}

static int
sg_last_dev(void)
{
	int k = -1;
	unsigned long iflags;

	read_lock_irqsave(&sg_index_lock, iflags);
	idr_for_each(&sg_index_idr, sg_idr_max_id, &k);
	read_unlock_irqrestore(&sg_index_lock, iflags);
	return k + 1;		/* origin 1 */
}
#endif

/* must be called with sg_index_lock held */
static struct sg_device *
sg_lookup_dev(int dev)
{
	return idr_find(&sg_index_idr, dev);
}

/*
 * Returns valid pointer to a sg_device object on success or a negated
 * errno value on failure. Does not return NULL.
 */
static struct sg_device *
sg_get_dev(int dev)
{
	struct sg_device *sdp;
	unsigned long flags;

	read_lock_irqsave(&sg_index_lock, flags);
	sdp = sg_lookup_dev(dev);
	if (!sdp)
		sdp = ERR_PTR(-ENXIO);
	else if (atomic_read(&sdp->detaching)) {
		/* If sdp->detaching, then the refcount may already be 0, in
		 * which case it would be a bug to do kref_get().
		 */
		sdp = ERR_PTR(-ENODEV);
	} else
		kref_get(&sdp->d_ref);
	read_unlock_irqrestore(&sg_index_lock, flags);

	return sdp;
}

#ifdef CONFIG_SCSI_PROC_FS
static int sg_proc_seq_show_int(struct seq_file *s, void *v);

static int sg_proc_single_open_adio(struct inode *inode, struct file *file);
static ssize_t sg_proc_write_adio(struct file *filp, const char __user *buffer,
			          size_t count, loff_t *off);
static const struct file_operations adio_fops = {
	.owner = THIS_MODULE,
	.open = sg_proc_single_open_adio,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = sg_proc_write_adio,
	.release = single_release,
};

static int sg_proc_single_open_dressz(struct inode *inode, struct file *file);
static ssize_t sg_proc_write_dressz(struct file *filp, 
		const char __user *buffer, size_t count, loff_t *off);
static const struct file_operations dressz_fops = {
	.owner = THIS_MODULE,
	.open = sg_proc_single_open_dressz,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = sg_proc_write_dressz,
	.release = single_release,
};

static int sg_proc_seq_show_version(struct seq_file *s, void *v);
static int sg_proc_seq_show_devhdr(struct seq_file *s, void *v);
static int sg_proc_seq_show_dev(struct seq_file *s, void *v);
static void * dev_seq_start(struct seq_file *s, loff_t *pos);
static void * dev_seq_next(struct seq_file *s, void *v, loff_t *pos);
static void dev_seq_stop(struct seq_file *s, void *v);
static const struct seq_operations dev_seq_ops = {
	.start = dev_seq_start,
	.next  = dev_seq_next,
	.stop  = dev_seq_stop,
	.show  = sg_proc_seq_show_dev,
};

static int sg_proc_seq_show_devstrs(struct seq_file *s, void *v);
static const struct seq_operations devstrs_seq_ops = {
	.start = dev_seq_start,
	.next  = dev_seq_next,
	.stop  = dev_seq_stop,
	.show  = sg_proc_seq_show_devstrs,
};

#if 0	/* temporary to shorten big patch */
static int sg_proc_seq_show_debug(struct seq_file *s, void *v);
#endif		/* temporary to shorten big patch */
static const struct seq_operations debug_seq_ops = {
	.start = dev_seq_start,
	.next  = dev_seq_next,
	.stop  = dev_seq_stop,
#if 0	/* temporary to shorten big patch */
	.show  = sg_proc_seq_show_debug,
#endif		/* temporary to shorten big patch */
};

static int
sg_proc_init(void)
{
	struct proc_dir_entry *p;

	p = proc_mkdir("scsi/sg", NULL);
	if (!p)
		return 1;

	proc_create("allow_dio", S_IRUGO | S_IWUSR, p, &adio_fops);
	proc_create_seq("debug", S_IRUGO, p, &debug_seq_ops);
	proc_create("def_reserved_size", S_IRUGO | S_IWUSR, p, &dressz_fops);
	proc_create_single("device_hdr", S_IRUGO, p, sg_proc_seq_show_devhdr);
	proc_create_seq("devices", S_IRUGO, p, &dev_seq_ops);
	proc_create_seq("device_strs", S_IRUGO, p, &devstrs_seq_ops);
	proc_create_single("version", S_IRUGO, p, sg_proc_seq_show_version);
	return 0;
}

static int
sg_proc_seq_show_int(struct seq_file *s, void *v)
{
	seq_printf(s, "%d\n", *((int *)s->private));
	return 0;
}

static int
sg_proc_single_open_adio(struct inode *inode, struct file *file)
{
	return single_open(file, sg_proc_seq_show_int, &sg_allow_dio);
}

static ssize_t 
sg_proc_write_adio(struct file *filp, const char __user *buffer,
		   size_t count, loff_t *off)
{
	int err;
	unsigned long num;

	if (!capable(CAP_SYS_ADMIN) || !capable(CAP_SYS_RAWIO))
		return -EACCES;
	err = kstrtoul_from_user(buffer, count, 0, &num);
	if (err)
		return err;
	sg_allow_dio = num ? 1 : 0;
	return count;
}

static int
sg_proc_single_open_dressz(struct inode *inode, struct file *file)
{
	return single_open(file, sg_proc_seq_show_int, &sg_big_buff);
}

static ssize_t 
sg_proc_write_dressz(struct file *filp, const char __user *buffer,
		     size_t count, loff_t *off)
{
	int err;
	unsigned long k = ULONG_MAX;

	if (!capable(CAP_SYS_ADMIN) || !capable(CAP_SYS_RAWIO))
		return -EACCES;

	err = kstrtoul_from_user(buffer, count, 0, &k);
	if (err)
		return err;
	if (k <= 1048576) {	/* limit "big buff" to 1 MB */
		sg_big_buff = k;
		return count;
	}
	return -ERANGE;
}

static int
sg_proc_seq_show_version(struct seq_file *s, void *v)
{
	seq_printf(s, "%d\t%s [%s]\n", sg_version_num, SG_VERSION_STR,
		   sg_version_date);
	return 0;
}

static int
sg_proc_seq_show_devhdr(struct seq_file *s, void *v)
{
	seq_puts(s, "host\tchan\tid\tlun\ttype\topens\tqdepth\tbusy\tonline\n");
	return 0;
}

struct sg_proc_deviter {
	loff_t	index;
	size_t	max;
};

static void *
dev_seq_start(struct seq_file *s, loff_t *pos)
{
	struct sg_proc_deviter * it = kmalloc(sizeof(*it), GFP_KERNEL);

	s->private = it;
	if (! it)
		return NULL;

	it->index = *pos;
	it->max = sg_last_dev();
	if (it->index >= it->max)
		return NULL;
	return it;
}

static void *
dev_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct sg_proc_deviter *it = s->private;

	*pos = ++it->index;
	return (it->index < it->max) ? it : NULL;
}

static void
dev_seq_stop(struct seq_file *s, void *v)
{
	kfree(s->private);
}

static int
sg_proc_seq_show_dev(struct seq_file *s, void *v)
{
	struct sg_proc_deviter *it = (struct sg_proc_deviter *)v;
	struct sg_device *sdp;
	struct scsi_device *scsidp;
	unsigned long iflags;

	read_lock_irqsave(&sg_index_lock, iflags);
	sdp = it ? sg_lookup_dev(it->index) : NULL;
	if (!sdp || !sdp->device || atomic_read(&sdp->detaching))
		seq_puts(s, "-1\t-1\t-1\t-1\t-1\t-1\t-1\t-1\t-1\n");
	else {
		scsidp = sdp->device;
		seq_printf(s, "%d\t%d\t%d\t%llu\t%d\t%d\t%d\t%d\t%d\n",
			      scsidp->host->host_no, scsidp->channel,
			      scsidp->id, scsidp->lun, (int)scsidp->type,
			      1,
			      (int)scsidp->queue_depth,
			      (int)atomic_read(&scsidp->device_busy),
			      (int)scsi_device_online(scsidp));
	}
	read_unlock_irqrestore(&sg_index_lock, iflags);
	return 0;
}

static int
sg_proc_seq_show_devstrs(struct seq_file *s, void *v)
{
	struct sg_proc_deviter *it = (struct sg_proc_deviter *)v;
	struct sg_device *sdp;
	struct scsi_device *scsidp;
	unsigned long iflags;

	read_lock_irqsave(&sg_index_lock, iflags);
	sdp = it ? sg_lookup_dev(it->index) : NULL;
	scsidp = sdp ? sdp->device : NULL;
	if (sdp && scsidp && (!atomic_read(&sdp->detaching)))
		seq_printf(s, "%8.8s\t%16.16s\t%4.4s\n",
			   scsidp->vendor, scsidp->model, scsidp->rev);
	else
		seq_puts(s, "<no active device>\n");
	read_unlock_irqrestore(&sg_index_lock, iflags);
	return 0;
}

#if 0	/* temporary to shorten big patch */

/* must be called while holding sg_index_lock */
static void
sg_proc_debug_helper(struct seq_file *s, struct sg_device *sdp)
{
	int k, new_interface, blen, usg;
	struct sg_request *srp;
	struct sg_fd *fp;
	const struct sg_io_hdr *hp;
	const char * cp;
	unsigned int ms;

	k = 0;
	list_for_each_entry(fp, &sdp->sfds, sfd_siblings) {
		k++;
		read_lock(&fp->rq_list_lock); /* irqs already disabled */
		seq_printf(s, "   FD(%d): timeout=%dms dlen=%d "
			   "(res)sgat=%d low_dma=%d\n", k,
			   jiffies_to_msecs(fp->timeout),
			   fp->reserve.dlen,
			   (int)fp->reserve.num_sgat,
			   (int)sdp->device->host->unchecked_isa_dma);
		seq_printf(s, "   cmd_q=%d f_packid=%d k_orphan=%d closed=0\n",
			   (int)fp->cmd_q, (int)fp->force_packid,
			   (int)fp->keep_orphan);
		list_for_each_entry(srp, &fp->rq_list, entry) {
			hp = &srp->header;
			new_interface = (hp->interface_id == '\0') ? 0 : 1;
			if (srp->res_used) {
				if (new_interface &&
				    (SG_FLAG_MMAP_IO & hp->flags))
					cp = "     mmap>> ";
				else
					cp = "     rb>> ";
			} else {
				if (SG_INFO_DIRECT_IO_MASK & hp->info)
					cp = "     dio>> ";
				else
					cp = "     ";
			}
			seq_puts(s, cp);
			blen = srp->data.dlen;
			usg = srp->data.num_sgat;
			seq_puts(s, srp->done ?
				 ((1 == srp->done) ?  "rcv:" : "fin:")
				  : "act:");
			seq_printf(s, " id=%d blen=%d",
				   srp->header.pack_id, blen);
			if (srp->done)
				seq_printf(s, " dur=%d", hp->duration);
			else {
				ms = jiffies_to_msecs(jiffies);
				seq_printf(s, " t_o/elap=%d/%d",
					(new_interface ? hp->timeout :
						  jiffies_to_msecs(fp->timeout)),
					(ms > hp->duration ? ms - hp->duration : 0));
			}
			seq_printf(s, "ms sgat=%d op=0x%02x\n", usg,
				   (int)srp->data.cmd_opcode);
		}
		if (list_empty(&fp->rq_list))
			seq_puts(s, "     No requests active\n");
		read_unlock(&fp->rq_list_lock);
	}
}

static int
sg_proc_seq_show_debug(struct seq_file *s, void *v)
{
	struct sg_proc_deviter *it = (struct sg_proc_deviter *)v;
	struct sg_device *sdp;
	unsigned long iflags;

	if (it && (0 == it->index))
		seq_printf(s, "max_active_device=%d  def_reserved_size=%d\n",
			   (int)it->max, sg_big_buff);

	read_lock_irqsave(&sg_index_lock, iflags);
	sdp = it ? sg_lookup_dev(it->index) : NULL;
	if (NULL == sdp)
		goto skip;
	read_lock(&sdp->sfd_lock);
	if (!list_empty(&sdp->sfds)) {
		seq_printf(s, " >>> device=%s ", sdp->disk->disk_name);
		if (atomic_read(&sdp->detaching))
			seq_puts(s, "detaching pending close ");
		else if (sdp->device) {
			struct scsi_device *scsidp = sdp->device;

			seq_printf(s, "%d:%d:%d:%llu   em=%d",
				   scsidp->host->host_no,
				   scsidp->channel, scsidp->id,
				   scsidp->lun,
				   scsidp->host->hostt->emulated);
		}
		seq_printf(s, " sg_tablesize=%d excl=%d open_cnt=%d\n",
			   sdp->sg_tablesize, sdp->exclude, sdp->open_cnt);
		sg_proc_debug_helper(s, sdp);
	}
	read_unlock(&sdp->sfd_lock);
skip:
	read_unlock_irqrestore(&sg_index_lock, iflags);
	return 0;
}
#endif		/* temporary to shorten big patch */

#endif				/* CONFIG_SCSI_PROC_FS */

module_init(init_sg);
module_exit(exit_sg);
