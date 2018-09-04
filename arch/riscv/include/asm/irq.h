/*
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2017 SiFive
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation, version 2.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 */

#ifndef _ASM_RISCV_IRQ_H
#define _ASM_RISCV_IRQ_H

/*
 * Possible interrupt causes:
 */
#define INTERRUPT_CAUSE_SOFTWARE    1
#define INTERRUPT_CAUSE_TIMER       5
#define INTERRUPT_CAUSE_EXTERNAL    9

/*
 * The high order bit of the trap cause register is always set for
 * interrupts, which allows us to differentiate them from exceptions
 * quickly.  The INTERRUPT_CAUSE_* macros don't contain that bit, so we
 * need to mask it off.
 */
#define INTERRUPT_CAUSE_FLAG	(1UL << (__riscv_xlen - 1))

void riscv_timer_interrupt(void);

#include <asm-generic/irq.h>

#endif /* _ASM_RISCV_IRQ_H */
