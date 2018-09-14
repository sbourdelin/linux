/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/* Copyright (c) 2018 Microsemi Corporation */
#ifndef __PHY_OCELOT_SERDES_H__
#define __PHY_OCELOT_SERDES_H__

#define SERDES1G_0	0
#define SERDES1G_1	1
#define SERDES1G_2	2
#define SERDES1G_3	3
#define SERDES1G_4	4
#define SERDES1G_5	5
#define SERDES1G_MAX	6
#define SERDES6G_0	SERDES1G_MAX
#define SERDES6G_1	(SERDES1G_MAX + 1)
#define SERDES6G_2	(SERDES1G_MAX + 2)
#define SERDES6G_MAX	(SERDES1G_MAX + 3)
#define SERDES_MAX	(SERDES1G_MAX + SERDES6G_MAX)

#endif
