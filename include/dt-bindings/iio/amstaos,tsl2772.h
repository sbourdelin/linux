/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Device driver for monitoring ambient light intensity (lux)
 * and proximity (prox) within the TAOS TSL2772 family of devices.
 *
 * Copyright (c) 2012 TAOS Corporation.
 * Copyright (c) 2017-2018 Brian Masney <masneyb@onstation.org>
 */

#ifndef _DT_BINDINGS_AMSTAOS_TSL2772_H
#define _DT_BINDINGS_AMSTAOS_TSL2772_H

/* Proximity diode to use */
#define TSL2772_DIODE0                  0x01
#define TSL2772_DIODE1                  0x02
#define TSL2772_DIODE_BOTH              0x03

/* LED Power */
#define TSL2772_100_mA                  0x00
#define TSL2772_50_mA                   0x01
#define TSL2772_25_mA                   0x02
#define TSL2772_13_mA                   0x03

#endif /* _DT_BINDINGS_AMSTAOS_TSL2772_H */
