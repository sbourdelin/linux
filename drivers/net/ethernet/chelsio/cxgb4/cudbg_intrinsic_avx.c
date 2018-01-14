/*
 *  Copyright (C) 2018 Chelsio Communications.  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms and conditions of the GNU General Public License,
 *  version 2, as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 *  more details.
 *
 *  The full GNU General Public License is included in this distribution in
 *  the file called "COPYING".
 *
 */

#include <linux/cpufeature.h>
#include <asm/fpu/api.h>

#include "cxgb4.h"
#include "cudbg_if.h"
#include "cudbg_lib_common.h"
#include "cudbg_intrinsic.h"

int cudbg_intrinsic_avx_supported(void)
{
#ifdef CONFIG_AS_AVX
	return boot_cpu_has(X86_FEATURE_AVX);
#else
	return 0;
#endif /* CONFIG_AS_AVX */
}

/* Alignment in bytes for AVX aligned instructions */
#define CUDBG_MEM_ALIGN_AVX 32

unsigned int cudbg_mem_read_avx(struct cudbg_init *pdbg_init, u32 start,
				u32 offset, u32 size, u32 mem_aperture,
				u8 *outbuf)
{
#ifdef CONFIG_AS_AVX
	u32 max_read_len = CUDBG_MEM_ALIGN_AVX;
	struct adapter *adap = pdbg_init->adap;
	u8 *reg_addr, *src_addr, *dst_addr;
	u32 bytes_read, read_len;

	reg_addr = (u8 *)adap->regs + start + offset;
	src_addr = PTR_ALIGN(reg_addr, max_read_len);
	dst_addr = PTR_ALIGN(outbuf, max_read_len);
	read_len = min(size, max_read_len);

	/* Don't use intrinsic for following cases:
	 * 1. If reading current offset + 256-bits would
	 *    exceed current window aperture.
	 * 2. Source or Destination address is not aligned
	 *    to 256-bits.
	 * 3. There are less than 256-bits left to read.
	 */
	if (offset + max_read_len > mem_aperture ||
	    src_addr != reg_addr || dst_addr != outbuf ||
	    read_len < max_read_len) {
		return cudbg_mem_read_def(pdbg_init, start, offset, size,
					  mem_aperture, outbuf);
	} else {
		kernel_fpu_begin();
		asm volatile("vmovdqa %0, %%ymm0" : : "m" (*reg_addr));
		asm volatile("vmovdqa %%ymm0, %0" : "=m" (*outbuf));
		kernel_fpu_end();
		bytes_read = read_len;
	}

	return bytes_read;
#else
	return cudbg_mem_read_def(pdbg_init, start, offset, size, mem_aperture,
				  outbuf);
#endif /* CONFIG_AS_AVX */
}
