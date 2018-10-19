/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Driver for the MSTAR 3367 HDMI Receiver
 *
 * Copyright (c) 2017 Steven Toth <stoth@kernellabs.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *
 * GNU General Public License for more details.
 */

#ifndef MST3367_H
#define MST3367_H

/* notify events */
#define MST3367_SOURCE_DETECT 0

struct mst3367_source_detect {
	int present;
};

#endif /* MST3367_H */
