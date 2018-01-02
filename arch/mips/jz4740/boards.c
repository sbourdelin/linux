// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic boards support
 * Copyright 2017, Paul Cercueil <paul@crapouillou.net>
 */

#include <asm/bootinfo.h>
#include <asm/mips_machine.h>

MIPS_MACHINE(MACH_INGENIC_JZ4740, "qi,lb60", "Qi Hardware Ben Nanonote", NULL);
MIPS_MACHINE(MACH_INGENIC_JZ4780, "img,ci20",
			"Imagination Technologies CI20", NULL);
