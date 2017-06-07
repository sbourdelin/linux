/*
 * Copyright 2016, Mikey Neuling, Chris Smart, IBM Corporation.
 * Copyright 2017, Michael Ellerman, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * Check that copy/paste works on Power9.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utils.h"


#define NUM_LOOPS 1000


/* This defines the "paste" instruction from Power ISA 3.0 Book II, section 4.4. */
#define PASTE(RA, RB, L, RC) \
	.long (0x7c00070c | (RA) << (31-15) | (RB) << (31-20) | (L) << (31-10) | (RC) << (31-31))

int paste(void *i)
{
	int cr;

	asm volatile(str(PASTE(0, %1, 1, 1))";"
			"mfcr %0;"
			: "=r" (cr)
			: "b" (i)
			: "memory"
		    );
	return cr;
}

/* This defines the "copy" instruction from Power ISA 3.0 Book II, section 4.4. */
#define COPY(RA, RB, L) \
	.long (0x7c00060c | (RA) << (31-15) | (RB) << (31-20) | (L) << (31-10))

void copy(void *i)
{
	asm volatile(str(COPY(0, %0, 1))";"
			:
			: "b" (i)
			: "memory"
		    );
}

int test_copy_paste(void)
{
	/* 128 bytes for a full cache line */
	char orig[128] __cacheline_aligned;
	char src[128] __cacheline_aligned;
	char dst[128] __cacheline_aligned;
	int rc;

	/* only run this test on a P9 or later */
	SKIP_IF(!have_hwcap2(PPC_FEATURE2_ARCH_3_00));

	memset(orig, 0x5a, sizeof(orig));
	memset(src, 0x5a, sizeof(src));
	memset(dst, 0x00, sizeof(dst));

	/* Confirm orig and src match */
	FAIL_IF(0 != memcmp(orig, src, sizeof(orig)));

	/* Confirm src & dst are different */
	FAIL_IF(0 == memcmp(src, dst, sizeof(src)));

	/*
	 * Paste can fail, eg. if we get context switched, so we do the
	 * copy/paste in a loop and fail the test if it never succeeds.
	 */
	for (int i = 0; i < NUM_LOOPS; i++) {
		copy(src);
		rc = paste(dst);

		/* A paste succeeds if CR0 EQ bit is set */
		if (rc & 0x20000000) {
			rc = 0;
			break;
		}
		rc = EAGAIN;
	}

	FAIL_IF(rc);

	/* Confirm orig and src still match */
	FAIL_IF(0 != memcmp(orig, src, sizeof(orig)));

	/* And that src and dst now match */
	FAIL_IF(0 != memcmp(src, dst, sizeof(src)));

	return 0;
}

int main(int argc, char *argv[])
{
	return test_harness(test_copy_paste, "copy_paste");
}
