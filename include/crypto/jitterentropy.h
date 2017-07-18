/*
 * Copyright (C) 2017, Stephan Mueller <smueller@chronox.de>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU General Public License, in which case the provisions of the GPL2 are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#ifndef _JITTERENTROPY_H
#define _JITTERENTROPY_H

typedef	unsigned long long	__u64;
typedef	long long		__s64;

/* The entropy pool */
struct rand_data {
	/*
	 * All data values that are vital to maintain the security
	 * of the RNG are marked as SENSITIVE. A user must not
	 * access that information while the RNG executes its loops to
	 * calculate the next random value.
	 */
	__u64 data;		/* SENSITIVE Actual random number */
	__u64 old_data;		/* SENSITIVE Previous random number */
	__u64 prev_time;	/* SENSITIVE Previous time stamp */
#define DATA_SIZE_BITS ((sizeof(__u64)) * 8)
	__u64 last_delta;	/* SENSITIVE stuck test */
	__s64 last_delta2;	/* SENSITIVE stuck test */
	unsigned int stuck:1;	/* Time measurement stuck */
	unsigned int osr;	/* Oversample rate */
	unsigned int stir:1;		/* Post-processing stirring */
	unsigned int disable_unbias:1;	/* Deactivate Von-Neuman unbias */
#define JENT_MEMORY_BLOCKS 64
#define JENT_MEMORY_BLOCKSIZE 32
#define JENT_MEMORY_ACCESSLOOPS 128
#define JENT_MEMORY_SIZE (JENT_MEMORY_BLOCKS*JENT_MEMORY_BLOCKSIZE)
	unsigned char *mem;	/*
				 * Memory access location with size of
				 * memblocks * memblocksize
				 */
	unsigned int memlocation; /* Pointer to byte in *mem */
	unsigned int memblocks;	/* Number of memory blocks in *mem */
	unsigned int memblocksize; /* Size of one memory block in bytes */
	unsigned int memaccessloops; /* Number of memory accesses per random
				      * bit generation */
};

int jent_entropy_init(void);
int jent_read_entropy(struct rand_data *ec, unsigned char *data,
		      unsigned int len);

#endif /* _JITTERENTROPY_H */
