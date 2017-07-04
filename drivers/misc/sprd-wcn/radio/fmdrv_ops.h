/*
 * SPREADTRUM SC2342 FM Radio driver
 *
 * Copyright (C) 2015~2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */


#ifndef _FMDRV_OPS_H
#define _FMDRV_OPS_H

extern struct fmdrv_ops *fmdev;
int  fm_device_init_driver(void);
void fm_device_exit_driver(void);

#endif /* _FMDRV_OPS_H */
