/*
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __ASM_MEMBLOCK_H
#define __ASM_MEMBLOCK_H

#ifdef CONFIG_ACPI
typedef struct {
	u64 base;
	u64 size;
	int resv;
} efi_acpi_reg_t;

#define MAX_ACPI_REGS   4
extern unsigned int nr_acpi_regs;
extern efi_acpi_reg_t acpi_regs[MAX_ACPI_REGS];
#endif

extern void arm64_memblock_init(void);
extern phys_addr_t memory_limit;
#endif
