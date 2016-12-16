#ifndef __SPARC_MMAN_H__
#define __SPARC_MMAN_H__

#include <uapi/asm/mman.h>

#ifndef __ASSEMBLY__
#define arch_mmap_check(addr,len,flags)	sparc_mmap_check(addr,len)
int sparc_mmap_check(unsigned long addr, unsigned long len);

#if defined(CONFIG_SHARED_MMU_CTX)
#define arch_shmat_check(file, shmflg, flags) \
				sparc_shmat_check(file, shmflg, flags)
int sparc_shmat_check(struct file *file, int shmflg, unsigned long *flags);
#endif
#endif
#endif /* __SPARC_MMAN_H__ */
