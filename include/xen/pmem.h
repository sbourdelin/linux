/******************************************************************************
 * pmem.h
 * pmem file for domain 0 kernel
 *
 * Copyright (c) 2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Haozhong Zhang <haozhong.zhang@intel.com>
 */

#ifndef __XEN_PMEM_H__
#define __XEN_PMEM_H__

#include <linux/types.h>

int xen_pmem_add(uint64_t spa, size_t size,
		 uint64_t rsv_off, size_t rsv_size,
		 uint64_t data_off, size_t data_size);

#endif /* __XEN_PMEM_H__ */
