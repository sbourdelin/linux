#ifndef _PKEYS_HELPER_H
#define _PKEYS_HELPER_H
#define _GNU_SOURCE
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <assert.h>
#include <stdlib.h>
#include <ucontext.h>
#include <sys/mman.h>

/* Define some kernel-like types */
#define  u8 uint8_t
#define u16 uint16_t
#define u32 uint32_t
#define u64 uint64_t

#ifdef __i386__ /* arch */

#define SYS_mprotect_key 380
#define SYS_pkey_alloc	 381
#define SYS_pkey_free	 382
#define REG_IP_IDX REG_EIP
#define si_pkey_offset 0x14

#define NR_PKEYS		16
#define NR_RESERVED_PKEYS	1
#define PKRU_BITS_PER_PKEY	2
#define PKEY_DISABLE_ACCESS	0x1
#define PKEY_DISABLE_WRITE	0x2
#define HPAGE_SIZE		(1UL<<21)

#define INIT_PRKU 0x0UL

#elif __powerpc64__ /* arch */

#define SYS_mprotect_key 386
#define SYS_pkey_alloc	 384
#define SYS_pkey_free	 385
#define si_pkey_offset	0x20
#define REG_IP_IDX PT_NIP
#define REG_TRAPNO PT_TRAP
#define REG_AMR		45
#define gregs gp_regs
#define fpregs fp_regs

#define NR_PKEYS		32
#define NR_RESERVED_PKEYS	3
#define PKRU_BITS_PER_PKEY	2
#define PKEY_DISABLE_ACCESS	0x3  /* disable read and write */
#define PKEY_DISABLE_WRITE	0x2
#define HPAGE_SIZE		(1UL<<24)

#define INIT_PRKU 0x3UL
#else /* arch */

	NOT SUPPORTED

#endif /* arch */


#ifndef DEBUG_LEVEL
#define DEBUG_LEVEL 0
#endif
#define DPRINT_IN_SIGNAL_BUF_SIZE 4096


static inline u32 pkey_to_shift(int pkey)
{
#ifdef __i386__ /* arch */
	return pkey * PKRU_BITS_PER_PKEY;
#elif __powerpc64__ /* arch */
	return (NR_PKEYS - pkey - 1) * PKRU_BITS_PER_PKEY;
#endif /* arch */
}


extern int dprint_in_signal;
extern char dprint_in_signal_buffer[DPRINT_IN_SIGNAL_BUF_SIZE];
static inline void sigsafe_printf(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	if (!dprint_in_signal) {
		vprintf(format, ap);
	} else {
		int len = vsnprintf(dprint_in_signal_buffer,
				    DPRINT_IN_SIGNAL_BUF_SIZE,
				    format, ap);
		/*
		 * len is amount that would have been printed,
		 * but actual write is truncated at BUF_SIZE.
		 */
		if (len > DPRINT_IN_SIGNAL_BUF_SIZE)
			len = DPRINT_IN_SIGNAL_BUF_SIZE;
		write(1, dprint_in_signal_buffer, len);
	}
	va_end(ap);
}
#define dprintf_level(level, args...) do {	\
	if (level <= DEBUG_LEVEL)		\
		sigsafe_printf(args);		\
	fflush(NULL);				\
} while (0)
#define dprintf0(args...) dprintf_level(0, args)
#define dprintf1(args...) dprintf_level(1, args)
#define dprintf2(args...) dprintf_level(2, args)
#define dprintf3(args...) dprintf_level(3, args)
#define dprintf4(args...) dprintf_level(4, args)

extern u64 shadow_pkey_reg;

static inline u64 __rdpkey_reg(void)
{
#ifdef __i386__ /* arch */
	unsigned int eax, edx;
	unsigned int ecx = 0;
	unsigned int pkey_reg;

	asm volatile(".byte 0x0f,0x01,0xee\n\t"
		     : "=a" (eax), "=d" (edx)
		     : "c" (ecx));
#elif __powerpc64__ /* arch */
	u64 eax;
	u64 pkey_reg;

	asm volatile("mfspr %0, 0xd" : "=r" ((u64)(eax)));
#endif /* arch */
	pkey_reg = (u64)eax;
	return pkey_reg;
}

static inline u64 _rdpkey_reg(int line)
{
	u64 pkey_reg = __rdpkey_reg();

	dprintf4("rdpkey_reg(line=%d) pkey_reg: %lx shadow: %lx\n",
			line, pkey_reg, shadow_pkey_reg);
	assert(pkey_reg == shadow_pkey_reg);

	return pkey_reg;
}

#define rdpkey_reg() _rdpkey_reg(__LINE__)

static inline void __wrpkey_reg(u64 pkey_reg)
{
#ifdef __i386__ /* arch */
	unsigned int eax = pkey_reg;
	unsigned int ecx = 0;
	unsigned int edx = 0;

	dprintf4("%s() changing %lx to %lx\n",
			 __func__, __rdpkey_reg(), pkey_reg);
	asm volatile(".byte 0x0f,0x01,0xef\n\t"
		     : : "a" (eax), "c" (ecx), "d" (edx));
	dprintf4("%s() PKRUP after changing %lx to %lx\n",
			__func__, __rdpkey_reg(), pkey_reg);
#else /* arch */
	u64 eax = pkey_reg;

	dprintf4("%s() changing %llx to %llx\n",
			 __func__, __rdpkey_reg(), pkey_reg);
	asm volatile("mtspr 0xd, %0" : : "r" ((unsigned long)(eax)) : "memory");
	dprintf4("%s() PKRUP after changing %llx to %llx\n",
			 __func__, __rdpkey_reg(), pkey_reg);
#endif /* arch */
	assert(pkey_reg == __rdpkey_reg());
}

static inline void wrpkey_reg(u64 pkey_reg)
{
	dprintf4("%s() changing %lx to %lx\n",
			__func__, __rdpkey_reg(), pkey_reg);
	/* will do the shadow check for us: */
	rdpkey_reg();
	__wrpkey_reg(pkey_reg);
	shadow_pkey_reg = pkey_reg;
	dprintf4("%s(%lx) pkey_reg: %lx\n",
		__func__, pkey_reg, __rdpkey_reg());
}

/*
 * These are technically racy. since something could
 * change PKRU between the read and the write.
 */
static inline void __pkey_access_allow(int pkey, int do_allow)
{
	u64 pkey_reg = rdpkey_reg();
	int bit = pkey * 2;

	if (do_allow)
		pkey_reg &= (1<<bit);
	else
		pkey_reg |= (1<<bit);

	dprintf4("pkey_reg now: %lx\n", rdpkey_reg());
	wrpkey_reg(pkey_reg);
}

static inline void __pkey_write_allow(int pkey, int do_allow_write)
{
	u64 pkey_reg = rdpkey_reg();
	int bit = pkey * 2 + 1;

	if (do_allow_write)
		pkey_reg &= (1<<bit);
	else
		pkey_reg |= (1<<bit);

	wrpkey_reg(pkey_reg);
	dprintf4("pkey_reg now: %lx\n", rdpkey_reg());
}

#define MB	(1<<20)

#ifdef __i386__ /* arch */

#define PAGE_SIZE 4096
static inline void __cpuid(unsigned int *eax, unsigned int *ebx,
		unsigned int *ecx, unsigned int *edx)
{
	/* ecx is often an input as well as an output. */
	asm volatile(
		"cpuid;"
		: "=a" (*eax),
		  "=b" (*ebx),
		  "=c" (*ecx),
		  "=d" (*edx)
		: "0" (*eax), "2" (*ecx));
}

/* Intel-defined CPU features, CPUID level 0x00000007:0 (ecx) */
#define X86_FEATURE_PKU        (1<<3) /* Protection Keys for Userspace */
#define X86_FEATURE_OSPKE      (1<<4) /* OS Protection Keys Enable */

static inline int cpu_has_pkey(void)
{
	unsigned int eax;
	unsigned int ebx;
	unsigned int ecx;
	unsigned int edx;

	eax = 0x7;
	ecx = 0x0;
	__cpuid(&eax, &ebx, &ecx, &edx);

	if (!(ecx & X86_FEATURE_PKU)) {
		dprintf2("cpu does not have PKU\n");
		return 0;
	}
	if (!(ecx & X86_FEATURE_OSPKE)) {
		dprintf2("cpu does not have OSPKE\n");
		return 0;
	}
	return 1;
}

#define XSTATE_PKRU_BIT	(9)
#define XSTATE_PKRU	0x200
int pkru_xstate_offset(void)
{
	unsigned int eax;
	unsigned int ebx;
	unsigned int ecx;
	unsigned int edx;
	int xstate_offset;
	int xstate_size;
	unsigned long XSTATE_CPUID = 0xd;
	int leaf;

	/* assume that XSTATE_PKRU is set in XCR0 */
	leaf = XSTATE_PKRU_BIT;
	{
		eax = XSTATE_CPUID;
		ecx = leaf;
		__cpuid(&eax, &ebx, &ecx, &edx);

		if (leaf == XSTATE_PKRU_BIT) {
			xstate_offset = ebx;
			xstate_size = eax;
		}
	}

	if (xstate_size == 0) {
		printf("could not find size/offset of PKRU in xsave state\n");
		return 0;
	}

	return xstate_offset;
}

/* 8-bytes of instruction * 512 bytes = 1 page */
#define __page_o_noops() asm(".rept 512 ; nopl 0x7eeeeeee(%eax) ; .endr")

#elif __powerpc64__ /* arch */

#define PAGE_SIZE (0x1UL << 16)
static inline int cpu_has_pkey(void)
{
	return 1;
}

/* 8-bytes of instruction * 16384bytes = 1 page */
#define __page_o_noops() asm(".rept 16384 ; nop; .endr")

#endif /* arch */

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*(x)))
#define ALIGN_UP(x, align_to)	(((x) + ((align_to)-1)) & ~((align_to)-1))
#define ALIGN_DOWN(x, align_to) ((x) & ~((align_to)-1))
#define ALIGN_PTR_UP(p, ptr_align_to)	\
		((typeof(p))ALIGN_UP((unsigned long)(p), ptr_align_to))
#define ALIGN_PTR_DOWN(p, ptr_align_to) \
	((typeof(p))ALIGN_DOWN((unsigned long)(p), ptr_align_to))
#define __stringify_1(x...)     #x
#define __stringify(x...)       __stringify_1(x)

#define PTR_ERR_ENOTSUP ((void *)-ENOTSUP)

extern void abort_hooks(void);
#define pkey_assert(condition) do {		\
	if (!(condition)) {			\
		dprintf0("assert() at %s::%d test_nr: %d iteration: %d\n", \
				__FILE__, __LINE__,	\
				test_nr, iteration_nr);	\
		dprintf0("errno at assert: %d", errno);	\
		abort_hooks();			\
		assert(condition);		\
	}					\
} while (0)
#define raw_assert(cond) assert(cond)


static inline int open_hugepage_file(int flag)
{
	int fd;
#ifdef __i386__ /* arch */
	fd = open("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages",
		 O_RDONLY);
#elif __powerpc64__ /* arch */
	fd = open("/sys/kernel/mm/hugepages/hugepages-16384kB/nr_hugepages",
		O_RDONLY);
#else /* arch */
	NOT SUPPORTED
#endif /* arch */
	return fd;
}

static inline int get_start_key(void)
{
#ifdef __i386__ /* arch */
	return 1;
#elif __powerpc64__ /* arch */
	return 0;
#else /* arch */
	NOT SUPPORTED
#endif /* arch */
}

#endif /* _PKEYS_HELPER_H */
