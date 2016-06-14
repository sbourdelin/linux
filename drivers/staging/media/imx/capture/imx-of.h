/*
 * Video Camera Capture driver for Freescale i.MX5/6 SOC
 *
 * Open Firmware parsing.
 *
 * Copyright (c) 2016 Mentor Graphics Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _IMX_CAM_OF_H
#define _IMX_CAM_OF_H

extern int imxcam_of_parse(struct imxcam_dev *dev, struct device_node *np);

#endif
