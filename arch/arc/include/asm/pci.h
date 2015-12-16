/*
 * Copyright (C) 2015-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _ASM_ARC_PCI_H
#define _ASM_ARC_PCI_H

#ifdef __KERNEL__
#include <asm-generic/pci-dma-compat.h>
#include <asm-generic/pci-bridge.h>

#include <asm/mach/pci.h> /* for pci_sys_data */

extern unsigned long pcibios_min_io;
#define PCIBIOS_MIN_IO pcibios_min_io
extern unsigned long pcibios_min_mem;
#define PCIBIOS_MIN_MEM pcibios_min_mem

#define pcibios_assign_all_busses()	1
/*
 * The PCI address space does equal the physical memory address space.
 * The networking and block device layers use this boolean for bounce
 * buffer decisions.
 */
#define PCI_DMA_BUS_IS_PHYS     (1)

#endif /* __KERNEL__ */

#endif /* _ASM_ARC_PCI_H */

