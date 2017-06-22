/*
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef OCC_COMMON_H
#define OCC_COMMON_H

#include <linux/hwmon-sysfs.h>
#include <linux/sysfs.h>

#define OCC_RESP_DATA_BYTES		4089

#define OCC_TIMEOUT_MS			5000
#define OCC_CMD_IN_PRG_MS		100

/* OCC return codes */
#define RESP_RETURN_CMD_IN_PRG		0xFF
#define RESP_RETURN_SUCCESS		0
#define RESP_RETURN_CMD_INVAL		0x11
#define RESP_RETURN_CMD_LEN		0x12
#define RESP_RETURN_DATA_INVAL		0x13
#define RESP_RETURN_CHKSUM		0x14
#define RESP_RETURN_OCC_ERR		0x15
#define RESP_RETURN_STATE		0x16

/* Same response format for all OCC versions.
 * Allocate the largest possible response.
 */
struct occ_response {
	u8 seq_no;
	u8 cmd_type;
	u8 return_status;
	u16 data_length_be;
	u8 data[OCC_RESP_DATA_BYTES];
	u16 checksum_be;
} __packed;

struct occ {
	struct device *bus_dev;

	struct occ_response resp;

	u8 poll_cmd_data;		/* to perform OCC poll command */
	int (*send_cmd)(struct occ *occ, u8 *cmd);
};

int occ_setup(struct occ *occ, const char *name);

#endif /* OCC_COMMON_H */
