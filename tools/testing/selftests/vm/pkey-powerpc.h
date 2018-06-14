/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _PKEYS_POWERPC_H
#define _PKEYS_POWERPC_H

#ifndef SYS_mprotect_key
# define SYS_mprotect_key	386
#endif
#ifndef SYS_pkey_alloc
# define SYS_pkey_alloc		384
# define SYS_pkey_free		385
#endif
#define REG_IP_IDX		PT_NIP
#define REG_TRAPNO		PT_TRAP
#define gregs			gp_regs
#define fpregs			fp_regs
#define si_pkey_offset		0x20

#ifndef PKEY_DISABLE_ACCESS
# define PKEY_DISABLE_ACCESS	0x3  /* disable read and write */
#endif

#ifndef PKEY_DISABLE_WRITE
# define PKEY_DISABLE_WRITE	0x2
#endif

#define NR_PKEYS		32
#define NR_RESERVED_PKEYS_4K	26
#define NR_RESERVED_PKEYS_64K	3
#define PKEY_BITS_PER_PKEY	2
#define HPAGE_SIZE		(1UL << 24)
#define PAGE_SIZE		(1UL << 16)
#define pkey_reg_t		u64
#define PKEY_REG_FMT		"%016lx"
#define HUGEPAGE_FILE		"/sys/kernel/mm/hugepages/hugepages-16384kB/nr_hugepages"

static inline u32 pkey_bit_position(int pkey)
{
	return (NR_PKEYS - pkey - 1) * PKEY_BITS_PER_PKEY;
}

static inline pkey_reg_t __read_pkey_reg(void)
{
	pkey_reg_t pkey_reg;

	asm volatile("mfspr %0, 0xd" : "=r" (pkey_reg));

	return pkey_reg;
}

static inline void __write_pkey_reg(pkey_reg_t pkey_reg)
{
	pkey_reg_t eax = pkey_reg;

	dprintf4("%s() changing "PKEY_REG_FMT" to "PKEY_REG_FMT"\n",
			 __func__, __read_pkey_reg(), pkey_reg);

	asm volatile("mtspr 0xd, %0" : : "r" ((unsigned long)(eax)) : "memory");

	dprintf4("%s() pkey register after changing "PKEY_REG_FMT" to "
			PKEY_REG_FMT"\n", __func__, __read_pkey_reg(),
			pkey_reg);
}

static inline bool is_pkey_supported(void)
{
	/*
	 * No simple way to determine this.
	 * Lets try allocating a key and see if it succeeds.
	 */
	int ret = sys_pkey_alloc(0, 0);

	if (ret > 0) {
		sys_pkey_free(ret);
		return true;
	}
	return false;
}

static inline int arch_reserved_keys(void)
{
	if (sysconf(_SC_PAGESIZE) == 4096)
		return NR_RESERVED_PKEYS_4K;
	else
		return NR_RESERVED_PKEYS_64K;
}

void expect_fault_on_read_execonly_key(void *p1, u16 pkey)
{
	/* powerpc does not allow userspace to change permissions of exec-only
	 * keys since those keys are not allocated by userspace. The signal
	 * handler wont be able to reset the permissions, which means the code
	 * will infinitely continue to segfault here.
	 */
	return;
}

/* 8-bytes of instruction * 16384bytes = 1 page */
#define __page_o_noops() asm(".rept 16384 ; nop; .endr")

void *malloc_pkey_with_mprotect_subpage(long size, int prot, u16 pkey)
{
	void *ptr;
	int ret;

	dprintf1("doing %s(size=%ld, prot=0x%x, pkey=%d)\n", __func__,
			size, prot, pkey);
	pkey_assert(pkey < NR_PKEYS);
	ptr = mmap(NULL, size, prot, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	pkey_assert(ptr != (void *)-1);

	ret = syscall(__NR_subpage_prot, ptr, size, NULL);
	if (ret) {
		perror("subpage_perm");
		return PTR_ERR_ENOTSUP;
	}

	ret = mprotect_pkey((void *)ptr, PAGE_SIZE, prot, pkey);
	pkey_assert(!ret);
	record_pkey_malloc(ptr, size, prot);

	dprintf1("%s() for pkey %d @ %p\n", __func__, pkey, ptr);
	return ptr;
}

#endif /* _PKEYS_POWERPC_H */
