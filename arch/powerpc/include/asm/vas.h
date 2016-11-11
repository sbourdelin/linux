/*
 * Copyright 2016 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#ifndef VAS_H
#define VAS_H

#define VAS_RX_FIFO_SIZE_MAX	(8 << 20)	/* 8MB */
/*
 * Co-processor Engine type.
 */
enum vas_cop_type {
	VAS_COP_TYPE_FAULT,
	VAS_COP_TYPE_842,
	VAS_COP_TYPE_842_HIPRI,
	VAS_COP_TYPE_GZIP,
	VAS_COP_TYPE_GZIP_HIPRI,
	VAS_COP_TYPE_MAX,
};

/*
 * Threshold Control Mode: Have paste operation fail if the number of
 * requests in receive FIFO exceeds a threshold.
 *
 * NOTE: No special error code yet if paste is rejected because of these
 *	 limits. So users can't distinguish between this and other errors.
 */
enum vas_thresh_ctl {
	VAS_THRESH_DISABLED,
	VAS_THRESH_FIFO_GT_HALF_FULL,
	VAS_THRESH_FIFO_GT_QTR_FULL,
	VAS_THRESH_FIFO_GT_EIGHTH_FULL,
};

#endif
