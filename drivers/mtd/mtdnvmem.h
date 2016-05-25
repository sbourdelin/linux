/*
 * Copyright (c) 2016, National Instruments Corp.
 *
 * Generic NVMEM support for OTP regions in MTD devices
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef MTDNVMEM_H
#define MTDNVMEM_H

struct mtd_nvmem;

struct mtd_nvmem *mtd_otp_nvmem_register(struct mtd_info *info);

void mtd_otp_nvmem_remove(struct mtd_nvmem *mem);

#endif /* MTDNVMEM_H */
