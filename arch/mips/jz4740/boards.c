/*
 * Ingenic boards support
 *
 * Copyright 2017, Paul Cercueil <paul@crapouillou.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or later
 * as published by the Free Software Foundation.
 */

#include <asm/bootinfo.h>
#include <asm/mips_machine.h>

MIPS_MACHINE(MACH_INGENIC_JZ4740, "qi,lb60", "Qi Hardware Ben Nanonote", NULL);
MIPS_MACHINE(MACH_INGENIC_JZ4780, "img,ci20",
			"Imagination Technologies CI20", NULL);
