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

#include <linux/linkage.h>

#include "hyp_head.S"

	.text
/**
 * void kvm_restore_host_vfp_state(struct vcpu *vcpu) -
 *     This function saves the guest, restores host, called from host.
 */
ENTRY(kvm_restore_host_vfp_state)
	push	xzr, lr

	add	x2, x0, #VCPU_CONTEXT
	bl __save_fpsimd

	ldr	x2, [x0, #VCPU_HOST_CONTEXT]
	bl __restore_fpsimd

	pop	xzr, lr
	ret
ENDPROC(kvm_restore_host_vfp_state)
