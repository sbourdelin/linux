/*
 * Copyright (C) 2017 National Instruments Corp
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef MFD_CROS_EC_H
#define MFD_CROS_EC_H

/**
 * cros_ec_readmem - Read mapped memory in the ChromeOS EC
 *
 * @ec: Device to read from
 * @offset: Offset to read within the mapped region
 * @bytes: number of bytes to read
 * @data: Return data
 * @return: 0 if Ok, -ve on error
 */
int cros_ec_readmem(struct cros_ec_device *ec, unsigned int offset,
		    unsigned int bytes, void *dest);

#endif /* MFD_CROS_EC_H */
