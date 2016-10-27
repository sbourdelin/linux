
/*
 *    Copyright (C) 2004 Benjamin Herrenschmidt, IBM Corp.
 *			 <benh@kernel.crashing.org>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/stddef.h>
#include <linux/unistd.h>
#include <linux/slab.h>
#include <linux/user.h>
#include <linux/elf.h>
#include <linux/security.h>
#include <linux/memblock.h>

#include <asm/cpu_has_feature.h>
#include <asm/pgtable.h>
#include <asm/processor.h>
#include <asm/mmu.h>
#include <asm/mmu_context.h>
#include <asm/prom.h>
#include <asm/machdep.h>
#include <asm/cputable.h>
#include <asm/sections.h>
#include <asm/firmware.h>
#include <asm/vdso.h>
#include <asm/vdso_datapage.h>
#include <asm/setup.h>

#undef DEBUG

#ifdef DEBUG
#define DBG(fmt...) printk(fmt)
#else
#define DBG(fmt...)
#endif

/* Max supported size for symbol names */
#define MAX_SYMNAME	64

/* The alignment of the vDSO */
#define VDSO_ALIGNMENT	(1 << 16)

static unsigned int vdso32_pages;
static struct vm_special_mapping vdso32_mapping;
unsigned long vdso32_sigtramp;
unsigned long vdso32_rt_sigtramp;

#ifdef CONFIG_VDSO32
extern char vdso32_start, vdso32_end;
static void *vdso32_kbase;
#endif

#ifdef CONFIG_PPC64
extern char vdso64_start, vdso64_end;
static void *vdso64_kbase = &vdso64_start;
static unsigned int vdso64_pages;
static struct vm_special_mapping vdso64_mapping;
unsigned long vdso64_rt_sigtramp;
#endif /* CONFIG_PPC64 */

static int vdso_ready;

/*
 * The vdso data page (aka. systemcfg for old ppc64 fans) is here.
 * Once the early boot kernel code no longer needs to muck around
 * with it, it will become dynamically allocated
 */
static union {
	struct vdso_data	data;
	u8			page[PAGE_SIZE];
} vdso_data_store __page_aligned_data;
struct vdso_data *vdso_data = &vdso_data_store.data;

/* Format of the patch table */
struct vdso_patch_def
{
	unsigned long	ftr_mask, ftr_value;
	const char	*gen_name;
	const char	*fix_name;
};

/* Table of functions to patch based on the CPU type/revision
 *
 * Currently, we only change sync_dicache to do nothing on processors
 * with a coherent icache
 */
static struct vdso_patch_def vdso_patches[] = {
	{
		CPU_FTR_COHERENT_ICACHE, CPU_FTR_COHERENT_ICACHE,
		"__kernel_sync_dicache", "__kernel_sync_dicache_p5"
	},
	{
		CPU_FTR_USE_TB, 0,
		"__kernel_gettimeofday", NULL
	},
	{
		CPU_FTR_USE_TB, 0,
		"__kernel_clock_gettime", NULL
	},
	{
		CPU_FTR_USE_TB, 0,
		"__kernel_clock_getres", NULL
	},
	{
		CPU_FTR_USE_TB, 0,
		"__kernel_get_tbfreq", NULL
	},
	{
		CPU_FTR_USE_TB, 0,
		"__kernel_time", NULL
	},
};

/*
 * Some infos carried around for each of them during parsing at
 * boot time.
 */
struct lib32_elfinfo
{
	Elf32_Ehdr	*hdr;		/* ptr to ELF */
	Elf32_Sym	*dynsym;	/* ptr to .dynsym section */
	unsigned long	dynsymsize;	/* size of .dynsym section */
	char		*dynstr;	/* ptr to .dynstr section */
	unsigned long	text;		/* offset of .text section in .so */
};

struct lib64_elfinfo
{
	Elf64_Ehdr	*hdr;
	Elf64_Sym	*dynsym;
	unsigned long	dynsymsize;
	char		*dynstr;
	unsigned long	text;
};

static int vdso_mremap(const struct vm_special_mapping *sm,
		struct vm_area_struct *new_vma)
{
	unsigned long new_size = new_vma->vm_end - new_vma->vm_start;
	unsigned long vdso_pages;

	if (is_32bit_task())
		vdso_pages = vdso32_pages;
#ifdef CONFIG_PPC64
	else
		vdso_pages = vdso64_pages;
#endif

	/* Do not allow partial remap, +1 is for vDSO data page */
	if (new_size != (vdso_pages + 1) << PAGE_SHIFT)
		return -EINVAL;

	if (WARN_ON_ONCE(current->mm != new_vma->vm_mm))
		return -EFAULT;

	current->mm->context.vdso_base = new_vma->vm_start;

	return 0;
}

static int map_vdso(struct vm_special_mapping *vsm, unsigned long vdso_pages,
		unsigned long vdso_base)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	int ret = 0;

	mm->context.vdso_base = 0;

	/*
	 * vDSO has a problem and was disabled, just don't "enable" it for the
	 * process
	 */
	if (vdso_pages == 0)
		return 0;

	/* Add a page to the vdso size for the data page */
	vdso_pages++;

	/*
	 * pick a base address for the vDSO in process space. We try to put it
	 * at vdso_base which is the "natural" base for it, but we might fail
	 * and end up putting it elsewhere.
	 * Add enough to the size so that the result can be aligned.
	 */
	if (down_write_killable(&mm->mmap_sem))
		return -EINTR;
	vdso_base = get_unmapped_area(NULL, vdso_base,
				      (vdso_pages << PAGE_SHIFT) +
				      ((VDSO_ALIGNMENT - 1) & PAGE_MASK),
				      0, 0);
	if (IS_ERR_VALUE(vdso_base)) {
		ret = vdso_base;
		goto out_up_mmap_sem;
	}

	/* Add required alignment. */
	vdso_base = ALIGN(vdso_base, VDSO_ALIGNMENT);

	/*
	 * our vma flags don't have VM_WRITE so by default, the process isn't
	 * allowed to write those pages.
	 * gdb can break that with ptrace interface, and thus trigger COW on
	 * those pages but it's then your responsibility to never do that on
	 * the "data" page of the vDSO or you'll stop getting kernel updates
	 * and your nice userland gettimeofday will be totally dead.
	 * It's fine to use that for setting breakpoints in the vDSO code
	 * pages though.
	 */
	vma = _install_special_mapping(mm, vdso_base, vdso_pages << PAGE_SHIFT,
				     VM_READ|VM_EXEC|
				     VM_MAYREAD|VM_MAYWRITE|VM_MAYEXEC,
				     vsm);
	if (IS_ERR(vma))
		ret = PTR_ERR(vma);
	else
		current->mm->context.vdso_base = vdso_base;

out_up_mmap_sem:
	up_write(&mm->mmap_sem);
	return ret;
}

/*
 * This is called from binfmt_elf, we create the special vma for the
 * vDSO and insert it into the mm struct tree
 */
int arch_setup_additional_pages(struct linux_binprm *bprm, int uses_interp)
{
	if (!vdso_ready)
		return 0;

	if (is_32bit_task())
		return map_vdso(&vdso32_mapping, vdso32_pages, VDSO32_MBASE);
#ifdef CONFIG_PPC64
	else
		/*
		 * On 64bit we don't have a preferred map address. This
		 * allows get_unmapped_area to find an area near other mmaps
		 * and most likely share a SLB entry.
		 */
		return map_vdso(&vdso64_mapping, vdso64_pages, 0);
#endif
	WARN_ONCE(1, "task is not 32-bit on non PPC64 kernel");
	return -1;
}

#ifdef CONFIG_VDSO32
#include "vdso_common.c"
#endif /* CONFIG_VDSO32 */

#ifdef CONFIG_PPC64
#define BITS 64
#include "vdso_common.c"
#endif /* CONFIG_PPC64 */


static __init void vdso_setup_trampolines(struct lib32_elfinfo *v32,
					  struct lib64_elfinfo *v64)
{
	/*
	 * Find signal trampolines
	 */

#ifdef CONFIG_PPC64
	vdso64_rt_sigtramp = find_function64(v64, "__kernel_sigtramp_rt64");
#endif
#ifdef CONFIG_VDSO32
	vdso32_sigtramp	   = find_function32(v32, "__kernel_sigtramp32");
	vdso32_rt_sigtramp = find_function32(v32, "__kernel_sigtramp_rt32");
#endif
}

static __init int vdso_fixup_alt_funcs(struct lib32_elfinfo *v32,
				       struct lib64_elfinfo *v64)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vdso_patches); i++) {
		struct vdso_patch_def *patch = &vdso_patches[i];
		int match = (cur_cpu_spec->cpu_features & patch->ftr_mask)
			== patch->ftr_value;
		if (!match)
			continue;

		DBG("replacing %s with %s...\n", patch->gen_name,
		    patch->fix_name ? "NONE" : patch->fix_name);

		/*
		 * Patch the 32 bits and 64 bits symbols. Note that we do not
		 * patch the "." symbol on 64 bits.
		 * It would be easy to do, but doesn't seem to be necessary,
		 * patching the OPD symbol is enough.
		 */
#ifdef CONFIG_VDSO32
		vdso_do_func_patch32(v32, patch->gen_name, patch->fix_name);
#endif
#ifdef CONFIG_PPC64
		vdso_do_func_patch64(v64, patch->gen_name, patch->fix_name);
#endif /* CONFIG_PPC64 */
	}

	return 0;
}

static __init int vdso_setup(void)
{
	struct lib32_elfinfo	v32;
	struct lib64_elfinfo	v64;

#ifdef CONFIG_VDSO32
	if (vdso_setup32(&v32))
		return -1;
#endif
#ifdef CONFIG_PPC64
	if (vdso_setup64(&v64))
		return -1;
#endif

	if (vdso_fixup_alt_funcs(&v32, &v64))
		return -1;

	vdso_setup_trampolines(&v32, &v64);

	return 0;
}

/*
 * Called from setup_arch to initialize the bitmap of available
 * syscalls in the systemcfg page
 */
static void __init vdso_setup_syscall_map(void)
{
	unsigned int i;
	extern unsigned long *sys_call_table;
	extern unsigned long sys_ni_syscall;


	for (i = 0; i < NR_syscalls; i++) {
#ifdef CONFIG_PPC64
		if (sys_call_table[i*2] != sys_ni_syscall)
			vdso_data->syscall_map_64[i >> 5] |=
				0x80000000UL >> (i & 0x1f);
		if (sys_call_table[i*2+1] != sys_ni_syscall)
			vdso_data->syscall_map_32[i >> 5] |=
				0x80000000UL >> (i & 0x1f);
#else /* CONFIG_PPC64 */
		if (sys_call_table[i] != sys_ni_syscall)
			vdso_data->syscall_map_32[i >> 5] |=
				0x80000000UL >> (i & 0x1f);
#endif /* CONFIG_PPC64 */
	}
}

#ifdef CONFIG_PPC64
int vdso_getcpu_init(void)
{
	unsigned long cpu, node, val;

	/*
	 * SPRG_VDSO contains the CPU in the bottom 16 bits and the NUMA node
	 * in the next 16 bits.  The VDSO uses this to implement getcpu().
	 */
	cpu = get_cpu();
	WARN_ON_ONCE(cpu > 0xffff);

	node = cpu_to_node(cpu);
	WARN_ON_ONCE(node > 0xffff);

	val = (cpu & 0xfff) | ((node & 0xffff) << 16);
	mtspr(SPRN_SPRG_VDSO_WRITE, val);
	get_paca()->sprg_vdso = val;

	put_cpu();

	return 0;
}
/* We need to call this before SMP init */
early_initcall(vdso_getcpu_init);
#endif

static int __init vdso_init(void)
{
#ifdef CONFIG_PPC64
	/*
	 * Fill up the "systemcfg" stuff for backward compatibility
	 */
	strcpy((char *)vdso_data->eye_catcher, "SYSTEMCFG:PPC64");
	vdso_data->version.major = SYSTEMCFG_MAJOR;
	vdso_data->version.minor = SYSTEMCFG_MINOR;
	vdso_data->processor = mfspr(SPRN_PVR);
	/*
	 * Fake the old platform number for pSeries and add
	 * in LPAR bit if necessary
	 */
	vdso_data->platform = 0x100;
	if (firmware_has_feature(FW_FEATURE_LPAR))
		vdso_data->platform |= 1;
	vdso_data->physicalMemorySize = memblock_phys_mem_size();
	vdso_data->dcache_size = ppc64_caches.dsize;
	vdso_data->dcache_line_size = ppc64_caches.dline_size;
	vdso_data->icache_size = ppc64_caches.isize;
	vdso_data->icache_line_size = ppc64_caches.iline_size;

	/* XXXOJN: Blocks should be added to ppc64_caches and used instead */
	vdso_data->dcache_block_size = ppc64_caches.dline_size;
	vdso_data->icache_block_size = ppc64_caches.iline_size;
	vdso_data->dcache_log_block_size = ppc64_caches.log_dline_size;
	vdso_data->icache_log_block_size = ppc64_caches.log_iline_size;

	/*
	 * Calculate the size of the 64 bits vDSO
	 */
	vdso64_pages = (&vdso64_end - &vdso64_start) >> PAGE_SHIFT;
	DBG("vdso64_kbase: %p, 0x%x pages\n", vdso64_kbase, vdso64_pages);
#else
	vdso_data->dcache_block_size = L1_CACHE_BYTES;
	vdso_data->dcache_log_block_size = L1_CACHE_SHIFT;
	vdso_data->icache_block_size = L1_CACHE_BYTES;
	vdso_data->icache_log_block_size = L1_CACHE_SHIFT;
#endif /* CONFIG_PPC64 */


#ifdef CONFIG_VDSO32
	vdso32_kbase = &vdso32_start;

	/*
	 * Calculate the size of the 32 bits vDSO
	 */
	vdso32_pages = (&vdso32_end - &vdso32_start) >> PAGE_SHIFT;
	DBG("vdso32_kbase: %p, 0x%x pages\n", vdso32_kbase, vdso32_pages);
#endif


	/*
	 * Setup the syscall map in the vDOS
	 */
	vdso_setup_syscall_map();

	/*
	 * Initialize the vDSO images in memory, that is do necessary
	 * fixups of vDSO symbols, locate trampolines, etc...
	 */
	if (vdso_setup()) {
		printk(KERN_ERR "vDSO setup failure, not enabled !\n");
		vdso32_pages = 0;
#ifdef CONFIG_PPC64
		vdso64_pages = 0;
#endif
		return 0;
	}

#ifdef CONFIG_VDSO32
	init_vdso32_pagelist();
#endif

#ifdef CONFIG_PPC64
	init_vdso64_pagelist();
#endif /* CONFIG_PPC64 */

	get_page(virt_to_page(vdso_data));

	smp_wmb();
	vdso_ready = 1;

	return 0;
}
arch_initcall(vdso_init);
