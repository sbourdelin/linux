/*
 * Copyright © 2016 Mellanox Technlogies. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef _NVME_PCI_H
#define _NVME_PCI_H

#include "nvme.h"

struct nvme_dev;

void *nvme_alloc_cmb(struct nvme_dev *dev, size_t size, dma_addr_t *dma_addr);
void nvme_free_cmb(struct nvme_dev *dev, void *addr, size_t size);

#endif
