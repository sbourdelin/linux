/*
 * Copyright (C) 2012,2013 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
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

#include <asm/alternative.h>
#include <asm/asm-offsets.h>
#include <asm/assembler.h>
#include <asm/cpufeature.h>
#include <asm/debug-monitors.h>
#include <asm/esr.h>
#include <asm/fpsimdmacros.h>
#include <asm/kvm.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_mmu.h>
#include <asm/memory.h>

#define CPU_GP_REG_OFFSET(x)    (CPU_GP_REGS + x)
#define CPU_XREG_OFFSET(x)      CPU_GP_REG_OFFSET(CPU_USER_PT_REGS + 8*x)
#define CPU_SPSR_OFFSET(x)      CPU_GP_REG_OFFSET(CPU_SPSR + 8*x)
#define CPU_SYSREG_OFFSET(x)    (CPU_SYSREGS + 8*x)

.macro save_fpsimd
	// x2: cpu context address
	// x3, x4: tmp regs
	add x3, x2, #CPU_GP_REG_OFFSET(CPU_FP_REGS)
	fpsimd_save x3, 4
.endm

.macro restore_fpsimd
	// x2: cpu context address
	// x3, x4: tmp regs
	add x3, x2, #CPU_GP_REG_OFFSET(CPU_FP_REGS)
	fpsimd_restore x3, 4
.endm
