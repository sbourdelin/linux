// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2018 - Arm Ltd

#ifndef __ARM64_KVM_RAS_H__
#define __ARM64_KVM_RAS_H__

#include <linux/acpi.h>
#include <linux/errno.h>
#include <linux/types.h>

#include <asm/acpi.h>

/*
 * Was this synchronous external abort a RAS notification?
 * Returns '0' for errors handled by some RAS subsystem, or -ENOENT.
 *
 * Call with irqs unmasked.
 */
static inline int kvm_handle_guest_sea(phys_addr_t addr, unsigned int esr)
{
	return apei_claim_sea(NULL);
}

#endif /* __ARM64_KVM_RAS_H__ */
