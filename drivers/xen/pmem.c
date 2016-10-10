/******************************************************************************
 * pmem.c
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

#include <linux/types.h>
#include <xen/interface/platform.h>
#include <asm/xen/hypercall.h>

int xen_pmem_add(uint64_t spa, size_t size,
		 uint64_t rsv_off, size_t rsv_size,
		 uint64_t data_off, size_t data_size)
{
	int rc;
	struct xen_platform_op op;

	if ((spa | size | rsv_off | rsv_size | data_off | data_size) &
	    (PAGE_SIZE - 1))
		return -EINVAL;

	op.cmd = XENPF_pmem_add;
	op.u.pmem_add.spfn = PHYS_PFN(spa);
	op.u.pmem_add.epfn = PHYS_PFN(spa) + PHYS_PFN(size);
	op.u.pmem_add.rsv_spfn = PHYS_PFN(spa + rsv_off);
	op.u.pmem_add.rsv_epfn = PHYS_PFN(spa + rsv_off + rsv_size);
	op.u.pmem_add.data_spfn = PHYS_PFN(spa + data_off);
	op.u.pmem_add.data_epfn = PHYS_PFN(spa + data_off + data_size);

	rc = HYPERVISOR_platform_op(&op);
	if (rc)
		pr_err("Xen pmem add failed on 0x%llx ~ 0x%llx, error: %d\n",
		       spa, spa + size, rc);

	return rc;
}
EXPORT_SYMBOL(xen_pmem_add);
