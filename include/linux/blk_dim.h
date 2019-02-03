/*
 * Copyright (c) 2016, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef BLK_DIM_H
#define BLK_DIM_H

#include <linux/module.h>
#include <linux/dim.h>

#define BLK_DIM_PARAMS_NUM_PROFILES 8
#define BLK_DIM_START_PROFILE 0

static const struct dim_cq_moder
blk_dim_prof[BLK_DIM_PARAMS_NUM_PROFILES] = {
	{1,   0, 1,  0},
	{2,   0, 2,  0},
	{4,   0, 4,  0},
	{16,  0, 4,  0},
	{32,  0, 4,  0},
	{32,  0, 16, 0},
	{256, 0, 16, 0},
	{256, 0, 32, 0},
};

void blk_dim(struct dim *dim, struct dim_sample end_sample);

#endif /* BLK_DIM_H */
