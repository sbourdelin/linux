/*
 * Copyright (C) 2015 - Samsung - Open Source Group
 * Author: Mario Smarduch <m.smarduch@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/linkage.h>
#include <linux/const.h>
#include <asm/unified.h>
#include <asm/page.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_arm.h>
#include <asm/vfpmacros.h>
#include "interrupts_head.S"

	.text
/**
  * void vcpu_restore_host_vfp_state(struct vcpu *vcpu) -
  * 	This function is called from host to save the guest, and restore host
  *     fp/simd hardware context.
  */
ENTRY(vcpu_restore_host_vfp_state)
#ifdef CONFIG_VFPv3
	push	{r4-r7}

	add	r7, r0, #VCPU_VFP_GUEST
	store_vfp_state r7

	add	r7, r0, #VCPU_VFP_HOST
	ldr	r7, [r7]
	restore_vfp_state r7

	pop	{r4-r7}
#endif
	bx	lr
ENDPROC(vcpu_restore_host_vfp_state)
