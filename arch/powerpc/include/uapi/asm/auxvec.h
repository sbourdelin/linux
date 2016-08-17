#ifndef _ASM_POWERPC_AUXVEC_H
#define _ASM_POWERPC_AUXVEC_H

/*
 * We need to put in some extra aux table entries to tell glibc what
 * the cache block size is, so it can use the dcbz instruction safely.
 */
#define AT_DCACHEBSIZE		19
#define AT_ICACHEBSIZE		20
#define AT_UCACHEBSIZE		21
/* A special ignored type value for PPC, for glibc compatibility.  */
#define AT_IGNOREPPC		22

/* The vDSO location. We have to use the same value as x86 for glibc's
 * sake :-)
 */
#define AT_SYSINFO_EHDR		33

/* More complete cache descriptions than AT_[DIU]CACHEBSIZE.  If the
   value is -1, then the cache doesn't exist or information isn't
   availble.  Otherwise:

      bit 0-9:	  Cache set-associativity 0 means fully associative.
      bit 10-13:  Log2 of cacheline size.
      bit 14-31:  Size of the entire cache >> 10.
      bit 32-39:  [64-bit only] more bits for total cache size.
      bit 40-63:  Reserved

   If any of the fields is all 1's, then that field isn't available.

    WARNING: The cache *line* size can be different from the cache *block*
             size. The latter, represented by vectors 19,20,21, is the size
	     used by the cache management instructions such as dcbz. The
	     cache line size on the other hand is the real HW line size
	     for a given cache level which might be different and should
	     only be used for performance related tuning
*/

#define AT_L1I_CACHESHAPE	34
#define AT_L1D_CACHESHAPE	35
#define AT_L2_CACHESHAPE	36
#define AT_L3_CACHESHAPE	37

#define AT_VECTOR_SIZE_ARCH	10 /* entries in ARCH_DLINFO */

#endif
