/*
 * Zoned block devices handling.
 *
 * Copyright (C) 2015 Seagate Technology PLC
 *
 * Written by: Shaun Tancheff <shaun.tancheff@seagate.com>
 *
 * Modified by: Damien Le Moal <damien.lemoal@hgst.com>
 * Copyright (C) 2016 Western Digital
 *
 * This file is licensed under  the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */
#ifndef _UAPI_BLKZONED_H
#define _UAPI_BLKZONED_H

#include <linux/types.h>
#include <linux/ioctl.h>

/*
 * Zone type.
 */
enum blkzone_type {
	BLKZONE_TYPE_UNKNOWN,
	BLKZONE_TYPE_CONVENTIONAL,
	BLKZONE_TYPE_SEQWRITE_REQ,
	BLKZONE_TYPE_SEQWRITE_PREF,
};

/*
 * Zone condition.
 */
enum blkzone_cond {
	BLKZONE_COND_NO_WP,
	BLKZONE_COND_EMPTY,
	BLKZONE_COND_IMP_OPEN,
	BLKZONE_COND_EXP_OPEN,
	BLKZONE_COND_CLOSED,
	BLKZONE_COND_READONLY = 0xd,
	BLKZONE_COND_FULL,
	BLKZONE_COND_OFFLINE,
};

/*
 * Zone descriptor for BLKREPORTZONE.
 * start, len and wp use the regulare 512 B sector unit,
 * regardless of the device logical block size. The overall
 * structure size is 64 B to match the ZBC/ZAC defined zone descriptor
 * and allow support for future additional zone information.
 */
struct blkzone {
       __u64 	start;	 	/* Zone start sector */
       __u64 	len;	 	/* Zone length in number of sectors */
       __u64 	wp;	 	/* Zone write pointer position */
       __u8	type;		/* Zone type */
       __u8	cond;		/* Zone condition */
       __u8	non_seq;	/* Non-sequential write resources active */
       __u8	reset;		/* Reset write pointer recommended */
       __u8 	reserved[36];
};

/*
 * Zone ioctl's:
 *
 * BLKUPDATEZONES	: Force update of all zones information
 * BLKREPORTZONE	: Get a zone descriptor. Takes a zone descriptor as
 *                        argument. The zone to report is the one
 *                        containing the sector initially specified in the
 *                        descriptor start field.
 * BLKRESETZONE		: Reset the write pointer of the zone containing the
 *                        specified sector, or of all written zones if the
 *                        sector is ~0ull.
 * BLKOPENZONE		: Explicitely open the zone containing the
 *                        specified sector, or all possible zones if the
 *                        sector is ~0ull (the drive determines which zone
 *                        to open in this case).
 * BLKCLOSEZONE		: Close the zone containing the specified sector, or
 *                        all open zones if the sector is ~0ull.
 * BLKFINISHZONE	: Finish the zone (make it full) containing the
 *                        specified sector, or all open and closed zones if
 *                        the sector is ~0ull.
 */
#define BLKUPDATEZONES	_IO(0x12,130)
#define BLKREPORTZONE 	_IOWR(0x12,131,struct blkzone)
#define BLKRESETZONE 	_IOW(0x12,132,unsigned long long)
#define BLKOPENZONE 	_IOW(0x12,133,unsigned long long)
#define BLKCLOSEZONE 	_IOW(0x12,134,unsigned long long)
#define BLKFINISHZONE 	_IOW(0x12,135,unsigned long long)

#endif /* _UAPI_BLKZONED_H */
