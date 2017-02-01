/*
 * Device Tree constants for Microsemi VSC8531 PHY
 *
 * Author: Nagaraju Lakkaraju
 *
 * License: Dual MIT/GPL
 * Copyright (c) 2017 Microsemi Corporation
 */

#ifndef _DT_BINDINGS_MSCC_VSC8531_H
#define _DT_BINDINGS_MSCC_VSC8531_H

/* PHY LED Modes */
#define LINK_ACTIVITY           0
#define LINK_1000_ACTIVITY      1
#define LINK_100_ACTIVITY       2
#define LINK_10_ACTIVITY        3
#define LINK_100_1000_ACTIVITY  4
#define LINK_10_1000_ACTIVITY   5
#define LINK_10_100_ACTIVITY    6
#define DUPLEX_COLLISION        8
#define COLLISION               9
#define ACTIVITY                10
#define AUTONEG_FAULT           12
#define SERIAL_MODE             13
#define FORCE_LED_OFF           14
#define FORCE_LED_ON            15

#endif
