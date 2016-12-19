/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1995 Linus Torvalds
 * Copyright (C) 1995 Waldorf Electronics
 * Copyright (C) 1994, 95, 96, 97, 98, 99, 2000, 01, 02, 03  Ralf Baechle
 * Copyright (C) 1996 Stoned Elipot
 * Copyright (C) 1999 Silicon Graphics, Inc.
 * Copyright (C) 2000, 2001, 2002, 2007	 Maciej W. Rozycki
 * Copyright (C) 2016 T-platforms
 */
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/export.h>
#include <linux/screen_info.h>
#include <linux/memblock.h>
#include <linux/bootmem.h>
#include <linux/initrd.h>
#include <linux/root_dev.h>
#include <linux/highmem.h>
#include <linux/console.h>
#include <linux/pfn.h>
#include <linux/debugfs.h>
#include <linux/kexec.h>
#include <linux/sizes.h>
#include <linux/device.h>
#include <linux/dma-contiguous.h>
#include <linux/decompress/generic.h>
#include <linux/of_fdt.h>

#include <asm/addrspace.h>
#include <asm/bootinfo.h>
#include <asm/bugs.h>
#include <asm/cache.h>
#include <asm/cdmm.h>
#include <asm/cpu.h>
#include <asm/debug.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp-ops.h>
#include <asm/prom.h>

#ifdef CONFIG_MIPS_ELF_APPENDED_DTB
const char __section(.appended_dtb) __appended_dtb[0x100000];
#endif /* CONFIG_MIPS_ELF_APPENDED_DTB */

struct cpuinfo_mips cpu_data[NR_CPUS] __read_mostly;

EXPORT_SYMBOL(cpu_data);

#ifdef CONFIG_VT
struct screen_info screen_info;
#endif

/*
 * Setup information
 *
 * These are initialized so they are in the .data section
 */
unsigned long mips_machtype __read_mostly = MACH_UNKNOWN;

EXPORT_SYMBOL(mips_machtype);

struct boot_mem_map boot_mem_map;

static char __initdata command_line[COMMAND_LINE_SIZE];
char __initdata arcs_cmdline[COMMAND_LINE_SIZE];

#ifdef CONFIG_CMDLINE_BOOL
static char __initdata builtin_cmdline[COMMAND_LINE_SIZE] = CONFIG_CMDLINE;
#endif

/*
 * mips_io_port_base is the begin of the address space to which x86 style
 * I/O ports are mapped.
 */
const unsigned long mips_io_port_base = -1;
EXPORT_SYMBOL(mips_io_port_base);

static struct resource code_resource = { .name = "Kernel code", };
static struct resource data_resource = { .name = "Kernel data", };

static void *detect_magic __initdata = detect_memory_region;

static phys_addr_t __initdata mips_lowmem_limit;

/*
 * General method to add RAM regions to the system
 *
 * NOTE Historically this method has been used to register memory blocks within
 *      MIPS kernel code in the boot_mem_map array. So we need to support it
 * up until it's discarded from platform-depended code.
 * On the other hand it might be good to have it, since we can check regions
 * before actually adding them
 */
void __init add_memory_region(phys_addr_t start, phys_addr_t size, long type)
{
	int x = boot_mem_map.nr_map;
	int ret, i;

	/*
	 * If the region reaches the top of the physical address space, adjust
	 * the size slightly so that (start + size) doesn't overflow
	 */
	if (start + size - 1 == (phys_addr_t)ULLONG_MAX)
		--size;

	/* Sanity check the region */
	if (start + size < start) {
		pr_warn("Trying to add an invalid memory region, skipped\n");
		return;
	}

	/* Make sure the type is supported */
	if (type != BOOT_MEM_RAM && type != BOOT_MEM_INIT_RAM &&
	    type != BOOT_MEM_ROM_DATA && type != BOOT_MEM_RESERVED) {
		pr_warn("Invalid type of memory region, skipped\n");
		return;
	}

	/*
	 * According to the request_resource logic RAM, INIT and ROM shouldn't
	 * intersect each other but being subset of one memory space
	 */
	if (type != BOOT_MEM_RESERVED && memblock_is_memory(start)) {
		pr_warn("Drop already added memory region %08zx @ %pa\n",
			(size_t)size, &start);
		return;
	}

	/*
	 * Add the region to the memblock allocator. Reserved regions should be
	 * in the memory as well to be actually reserved.
	 */
	ret = memblock_add_node(start, size, 0);
	if (ret < 0) {
		pr_err("Could't add memblock %08zx @ %pa\n",
			(size_t)size, &start);
		return;
	}

	/* Reserve memory region passed with the corresponding flags */
	if (type != BOOT_MEM_RAM) {
		ret = memblock_reserve(start, size);
		if (ret < 0) {
			pr_err("Could't reserve memblock %08zx @ %pa\n",
				(size_t)size, &start);
			return;
		}
	}

	/* Try to combine with existing entry, if any. */
	for (i = 0; i < boot_mem_map.nr_map; i++) {
		struct boot_mem_map_entry *entry = boot_mem_map.map + i;
		unsigned long top;

		if (entry->type != type)
			continue;

		if (start + size < entry->addr)
			continue;			/* no overlap */

		if (entry->addr + entry->size < start)
			continue;			/* no overlap */

		top = max(entry->addr + entry->size, start + size);
		entry->addr = min(entry->addr, start);
		entry->size = top - entry->addr;

		return;
	}

	if (boot_mem_map.nr_map == BOOT_MEM_MAP_MAX) {
		pr_err("Ooops! Too many entries in the memory map!\n");
		return;
	}

	boot_mem_map.map[x].addr = start;
	boot_mem_map.map[x].size = size;
	boot_mem_map.map[x].type = type;
	boot_mem_map.nr_map++;
}

void __init detect_memory_region(phys_addr_t start, phys_addr_t sz_min, phys_addr_t sz_max)
{
	void *dm = &detect_magic;
	phys_addr_t size;

	for (size = sz_min; size < sz_max; size <<= 1) {
		if (!memcmp(dm, dm + size, sizeof(detect_magic)))
			break;
	}

	pr_debug("Memory: %lluMB of RAM detected at 0x%llx (min: %lluMB, max: %lluMB)\n",
		((unsigned long long) size) / SZ_1M,
		(unsigned long long) start,
		((unsigned long long) sz_min) / SZ_1M,
		((unsigned long long) sz_max) / SZ_1M);

	add_memory_region(start, size, BOOT_MEM_RAM);
}

/*
 * Print declared memory layout
 */
static void __init print_memory_map(void)
{
	int i;
	const int field = 2 * sizeof(unsigned long);

	/* Print the added memory map  */
	pr_info("Determined physical RAM map:\n");
	for (i = 0; i < boot_mem_map.nr_map; i++) {
		printk(KERN_INFO " memory: %0*Lx @ %0*Lx ",
		       field, (unsigned long long) boot_mem_map.map[i].size,
		       field, (unsigned long long) boot_mem_map.map[i].addr);

		switch (boot_mem_map.map[i].type) {
		case BOOT_MEM_RAM:
			printk(KERN_CONT "(usable)\n");
			break;
		case BOOT_MEM_INIT_RAM:
			printk(KERN_CONT "(usable after init)\n");
			break;
		case BOOT_MEM_ROM_DATA:
			printk(KERN_CONT "(ROM data)\n");
			break;
		case BOOT_MEM_RESERVED:
			printk(KERN_CONT "(reserved)\n");
			break;
		default:
			printk(KERN_CONT "type %lu\n", boot_mem_map.map[i].type);
			break;
		}
	}

	/* Print memblocks if memblock_debug is set */
	memblock_dump_all();
}

/*
 * Parse passed cmdline
 */
#define USE_PROM_CMDLINE	IS_ENABLED(CONFIG_MIPS_CMDLINE_FROM_BOOTLOADER)
#define USE_DTB_CMDLINE		IS_ENABLED(CONFIG_MIPS_CMDLINE_FROM_DTB)
#define EXTEND_WITH_PROM	IS_ENABLED(CONFIG_MIPS_CMDLINE_EXTEND)
#define BUILTIN_EXTEND_WITH_PROM	\
	IS_ENABLED(CONFIG_MIPS_CMDLINE_BUILTIN_EXTEND)

static void __init mips_parse_param(char **cmdline_p)
{
#if defined(CONFIG_CMDLINE_BOOL) && defined(CONFIG_CMDLINE_OVERRIDE)
	strlcpy(boot_command_line, builtin_cmdline, COMMAND_LINE_SIZE);
#else
	if ((USE_PROM_CMDLINE && arcs_cmdline[0]) ||
	    (USE_DTB_CMDLINE && !boot_command_line[0]))
		strlcpy(boot_command_line, arcs_cmdline, COMMAND_LINE_SIZE);

	if (EXTEND_WITH_PROM && arcs_cmdline[0]) {
		if (boot_command_line[0])
			strlcat(boot_command_line, " ", COMMAND_LINE_SIZE);
		strlcat(boot_command_line, arcs_cmdline, COMMAND_LINE_SIZE);
	}

#if defined(CONFIG_CMDLINE_BOOL)
	if (builtin_cmdline[0]) {
		if (boot_command_line[0])
			strlcat(boot_command_line, " ", COMMAND_LINE_SIZE);
		strlcat(boot_command_line, builtin_cmdline, COMMAND_LINE_SIZE);
	}

	if (BUILTIN_EXTEND_WITH_PROM && arcs_cmdline[0]) {
		if (boot_command_line[0])
			strlcat(boot_command_line, " ", COMMAND_LINE_SIZE);
		strlcat(boot_command_line, arcs_cmdline, COMMAND_LINE_SIZE);
	}
#endif
#endif
	strlcpy(command_line, boot_command_line, COMMAND_LINE_SIZE);

	*cmdline_p = command_line;

	parse_early_param();
}

/*
 * Parse "mem=size@start" parameter rewriting a defined memory map
 * We look for mem=size@start, where start and size are "value[KkMm]"
 */
static int __init early_parse_mem(char *p)
{
	static int usermem;
	phys_addr_t start, size;

	start = PHYS_OFFSET;
	size = memparse(p, &p);
	if (*p == '@')
		start = memparse(p + 1, &p);

	/*
	 * If a user specifies memory size, we blow away any automatically
	 * generated regions.
	 */
	if (usermem == 0) {
		phys_addr_t ram_start = memblock_start_of_DRAM();
		phys_addr_t ram_end = memblock_end_of_DRAM() - ram_start;

		pr_notice("Discard memory layout %pa - %pa",
			  &ram_start, &ram_end);

		memblock_remove(ram_start, ram_end - ram_start);
		boot_mem_map.nr_map = 0;
		usermem = 1;
	}
	pr_notice("Add userdefined memory region %08zx @ %pa",
		  (size_t)size, &start);

	add_memory_region(start, size, BOOT_MEM_RAM);
	return 0;
}
early_param("mem", early_parse_mem);

/*
 * Helper method checking whether passed lowmem region is valid
 */
static bool __init is_lowmem_and_valid(const char *name, phys_addr_t base,
				       phys_addr_t size)
{
	phys_addr_t end = base + size;

	/* Check whether region belongs to actual memory */
	if (!memblock_is_region_memory(base, size)) {
		pr_err("%s %08zx @ %pa is not a memory region", name,
			(size_t)size, &base);
		return false;
	}

	/* Check whether region belongs to low memory */
	if (end > mips_lowmem_limit) {
		pr_err("%s %08zx @ %pa is out of low memory", name,
			(size_t)size, &base);
	       return false;
	}

	/* Check whether region is free */
	if (memblock_is_region_reserved(base, size)) {
		pr_err("%s %08zx @ %pa overlaps in-use memory", name,
			(size_t)size, &base);
		return false;
	}

	return true;
}

/*
 * Manage initrd
 */
#ifdef CONFIG_BLK_DEV_INITRD

static int __init rd_start_early(char *p)
{
	unsigned long start = memparse(p, &p);

#ifdef CONFIG_64BIT
	/* Guess if the sign extension was forgotten by bootloader */
	if (start < XKPHYS)
		start = (int)start;
#endif
	initrd_start = start;
	initrd_end += start;
	return 0;
}
early_param("rd_start", rd_start_early);

static int __init rd_size_early(char *p)
{
	initrd_end += memparse(p, &p);
	return 0;
}
early_param("rd_size", rd_size_early);

/* In some conditions (e.g. big endian bootloader with a little endian
   kernel), the initrd might appear byte swapped.  Try to detect this and
   byte swap it if needed.  */
static void __init maybe_bswap_initrd(void)
{
#if defined(CONFIG_CPU_CAVIUM_OCTEON)
	u64 buf;

	/* Check for CPIO signature */
	if (!memcmp((void *)initrd_start, "070701", 6))
		return;

	/* Check for compressed initrd */
	if (decompress_method((unsigned char *)initrd_start, 8, NULL))
		return;

	/* Try again with a byte swapped header */
	buf = swab64p((u64 *)initrd_start);
	if (!memcmp(&buf, "070701", 6) ||
	    decompress_method((unsigned char *)(&buf), 8, NULL)) {
		unsigned long i;

		pr_info("Byteswapped initrd detected\n");
		for (i = initrd_start; i < ALIGN(initrd_end, 8); i += 8)
			swab64s((u64 *)i);
	}
#endif
}

/*
 * Check and reserve memory occupied by initrd
 */
static void __init mips_reserve_initrd_mem(void)
{
	phys_addr_t phys_initrd_start, phys_initrd_end, phys_initrd_size;

	/*
	 * Board specific code or command line parser should have already set
	 * up initrd_start and initrd_end. In these cases perform sanity checks
	 * and use them if all looks good.
	 */
	if (!initrd_start || initrd_end <= initrd_start) {
		pr_info("No initrd found");
		goto disable;
	}
	if (initrd_start & ~PAGE_MASK) {
		pr_err("Initrd start must be page aligned");
		goto disable;
	}
	if (initrd_start < PAGE_OFFSET) {
		pr_err("Initrd start < PAGE_OFFSET");
		goto disable;
	}

	/*
	 * Sanitize initrd addresses. For example firmware can't guess if they
	 * need to pass them through 64-bits values if the kernel has been
	 * built in pure 32-bit. We need also to switch from KSEG0 to XKPHYS
	 * addresses now, so the code can now safely use __pa().
	 */
	phys_initrd_start = __pa(initrd_start);
	phys_initrd_end = __pa(initrd_end);
	phys_initrd_size = phys_initrd_end - phys_initrd_start;

	/* Check whether initrd region is within available lowmem and free */
	if (!is_lowmem_and_valid("Initrd", phys_initrd_start, phys_initrd_size))
		goto disable;

	/* Initrd may be byteswapped in Octeon */
	maybe_bswap_initrd();

	/* Memory for initrd can be reserved now */
	memblock_reserve(phys_initrd_start, phys_initrd_size);

	/* Convert initrd to virtual addresses back (needed for x32 -> x64) */
	initrd_start = (unsigned long)__va(phys_initrd_start);
	initrd_end = (unsigned long)__va(phys_initrd_end);

	/* It's OK to have initrd below actual memory start. Really? */
	initrd_below_start_ok = 1;

	pr_info("Initial ramdisk at: 0x%lx (%zu bytes)\n",
		initrd_start, (size_t)phys_initrd_size);

	/* Set root device to be first ram disk */
	ROOT_DEV = Root_RAM0;

	return;
disable:
	printk(KERN_CONT " - disabling initrd\n");
	initrd_start = 0;
	initrd_end = 0;
}

#else  /* !CONFIG_BLK_DEV_INITRD */

static void __init mips_reserve_initrd_mem(void) { }

#endif

/*
 * Check initialized memory.
 */
static void __init sanity_check_meminfo(void)
{
	phys_addr_t physmem_start = PFN_PHYS(ARCH_PFN_OFFSET);
	phys_addr_t size_limit = 0;
	struct memblock_region *reg;
	bool highmem = false;
	bool should_use_highmem = false;

	/*
	 * Walk over all memory ranges discarding highmem if it's disabled and
	 * calculating the memblock allocator limit
	 */
	for_each_memblock(memory, reg) {
		phys_addr_t block_start = reg->base;
		phys_addr_t block_end = reg->base + reg->size;
		phys_addr_t block_size = reg->size;

		if (block_start >= HIGHMEM_START) {
			highmem = true;
			size_limit = block_size;
		} else {
			size_limit = HIGHMEM_START - block_start;
		}

		/* Discard highmem physical memory if it isn't supported */
		if (!IS_BUILTIN(CONFIG_HIGHMEM)) {
			/* Discard the whole highmem memory block */
			if (highmem) {
				pr_notice("Ignoring RAM at %pa-%pa (!CONFIG_HIGHMEM)\n",
					&block_start, &block_end);
				memblock_remove(block_start, block_size);
				should_use_highmem = true;
				continue;
			}
			/* Truncate memory block */
			if (block_size > size_limit) {
				phys_addr_t overlap_size = block_size - size_limit;
				phys_addr_t highmem_start = HIGHMEM_START;

				pr_notice("Truncate highmem %pa-%pa to -%pa\n",
					&block_start, &block_end, &highmem_start);
				memblock_remove(highmem_start, overlap_size);
				block_end = highmem_start;
				should_use_highmem = true;
			}
		}
		/* Truncate region if it starts below ARCH_PFN_OFFSET */
		if (block_start < physmem_start) {
			phys_addr_t overlap_size = physmem_start - block_start;

			pr_notice("Truncate lowmem %pa-%pa to %pa-\n",
				&block_start, &block_end, &physmem_start);
			memblock_remove(block_start, overlap_size);
		}

		/* Calculate actual lowmem limit for memblock allocator */
		if (!highmem) {
			if (block_end > mips_lowmem_limit) {
				if (block_size > size_limit)
					mips_lowmem_limit = HIGHMEM_START;
				else
					mips_lowmem_limit = block_end;
			}
		}
	}

	/* Panic if no lowmem has been determined */
	if (!mips_lowmem_limit)
		panic("Oops, where is low memory? 0_o\n");

	if (should_use_highmem)
		pr_notice("Consider using HIGHMEM enabled kernel\n");

	/* Set memblock allocator limit */
	memblock_set_current_limit(mips_lowmem_limit);
}

/*
 * Reserve kernel code and data within memblock allocator
 */
static void __init mips_reserve_kernel_mem(void)
{
	phys_addr_t start, size;

	/*
	 * Add kernel _text, _data, _bss, __init*, upto __end sections to
	 * boot_mem_map and memblock. We must reserve all of them!
	 */
	start = __pa_symbol(&_text);
	size = __pa_symbol(&_end) - start;
	add_memory_region(start, size, BOOT_MEM_RAM);
	/*
	 * It needs to be reserved within memblock as well. It's ok if memory
	 * has already been reserved with previous method
	 */
	memblock_reserve(start, size);

	/* Reserve nosave region for hibernation */
	start = __pa_symbol(&__nosave_begin);
	size = __pa_symbol(&__nosave_end) - start;
	add_memory_region(start, size, BOOT_MEM_RAM);
	memblock_reserve(start, size);

	/* Initialize some init_mm fieldis. We may not need this? */
	init_mm.start_code = (unsigned long)&_text;
	init_mm.end_code = (unsigned long)&_etext;
	init_mm.end_data = (unsigned long)&_edata;
	init_mm.brk = (unsigned long)&_end;

	/*
	 * The kernel reserves all memory below its _end symbol as bootmem,
	 * but the kernel may now be at a much higher address. The memory
	 * between the original and new locations may be returned to the system.
	 */
#ifdef CONFIG_RELOCATABLE
	if (__pa_symbol(&_text) > __pa_symbol(VMLINUX_LOAD_ADDRESS)) {
		phys_addr_t offset;
		extern void show_kernel_relocation(const char *level);

		offset = __pa_symbol(_text) - __pa_symbol(VMLINUX_LOAD_ADDRESS);
		memblock_free(__pa_symbol(VMLINUX_LOAD_ADDRESS), offset);

#if defined(CONFIG_DEBUG_KERNEL) && defined(CONFIG_DEBUG_INFO)
		/*
		 * This information is necessary when debugging the kernel
		 * But is a security vulnerability otherwise!
		 */
		show_kernel_relocation(KERN_INFO);
#endif
	}
#endif
}

/*
 * Reserve memory occupied by elfcorehdr
 */
static void __init mips_reserve_elfcorehdr(void)
{
#ifdef CONFIG_PROC_VMCORE
	/*
	 * Don't reserve anything if kernel isn't booting after a panic and
	 * vmcore is usable (see linux/crash_dump.h for details)
	 */
	if (!is_vmcore_usable())
		return;

	/* Check whether the passed address belongs to low memory */
	if (elfcorehdr_addr + elfcorehdr_size >= mips_lowmem_limit) {
		pr_err("Elfcorehdr %08zx @ %pa doesn't belong to low memory",
			(size_t)elfcorehdr_size, &elfcorehdr_addr);
		return;
	}

	/*
	 * If elfcorehdr_size hasn't been specified, then try to reserve upto
	 * low memory limit
	 */
	if (!elfcorehdr_size)
		elfcorehdr_size = mips_lowmem_limit - elfcorehdr_addr;

	/* Check the region belongs to actual memory (size can be zero) */
	if (!memblock_is_region_memory(elfcorehdr_addr, elfcorehdr_size)) {
		pr_err("Elfcorehdr %08zx @ %pa is not a memory region",
			(size_t)elfcorehdr_size, &elfcorehdr_addr);
		return;
	}

	/* Check whether elfcorehdr region is free */
	if (memblock_is_region_reserved(elfcorehdr_addr, elfcorehdr_size)) {
		pr_err("Elfcorehdr %08zx @ %pa overlaps in-use memory",
			(size_t)elfcorehdr_size, &elfcorehdr_addr);
		return;
	}

	/* Reserve elfcorehdr within memblock */
	memblock_reserve(elfcorehdr_addr, PAGE_ALIGN(elfcorehdr_size));

	pr_info("Reserved memory for kdump at %08zx @ %pa\n",
		(size_t)elfcorehdr_size, &elfcorehdr_addr);
#endif /* CONFIG_PROC_VMCORE */
}

#ifdef CONFIG_KEXEC
/*
 * Parse passed crashkernel parameter and reserve corresponding memory
 */
static void __init mips_parse_crashkernel(void)
{
	unsigned long long total_mem;
	unsigned long long crash_size, crash_base;
	int ret;

	/* Parse crachkernel parameter */
	total_mem = memblock_phys_mem_size();
	ret = parse_crashkernel(boot_command_line, total_mem,
				&crash_size, &crash_base);
	if (ret != 0 || crash_size <= 0)
		return;

	crashk_res.start = crash_base;
	crashk_res.end	 = crash_base + crash_size - 1;

	/* Check whether the region belogs to lowmem and valid */
	if (!is_lowmem_and_valid("Crashkernel", crash_base, crash_size))
		return;

	/* Reserve crashkernel resource */
	memblock_reserve(crash_base, crash_size);
}

/*
 * Reserve crashkernel memory within passed RAM resource
 */
static void __init request_crashkernel(struct resource *res)
{
	int ret;

	ret = request_resource(res, &crashk_res);
	if (!ret)
		pr_info("Reserving %ldMB of memory at %ldMB for crashkernel\n",
			(unsigned long)((crashk_res.end -
					 crashk_res.start + 1) >> 20),
			(unsigned long)(crashk_res.start  >> 20));
}
#else /* !CONFIG_KEXEC */

static void __init mips_parse_crashkernel(void) { }
static void __init request_crashkernel(struct resource *res) { }

#endif /* !CONFIG_KEXEC */

/*
 * Calcualte PFN limits with respect to the defined memory layout
 */
static void __init find_pfn_limits(void)
{
	phys_addr_t ram_end = memblock_end_of_DRAM();

	min_low_pfn = ARCH_PFN_OFFSET;
	max_low_pfn = PFN_UP(HIGHMEM_START);
	max_pfn = PFN_UP(ram_end);
#ifdef CONFIG_HIGHMEM
	highstart_pfn = max_low_pfn;
	highend_pfn = max_pfn <= highstart_pfn ? highstart_pfn : max_pfn;
#endif
	pr_info("PFNs: low min %lu, low max %lu, high start %lu, high end %lu,"
		"max %lu\n",
		min_low_pfn, max_low_pfn, highstart_pfn, highend_pfn, max_pfn);
}

/*
 * Initialize the memblock allocator
 */
#if defined(CONFIG_SGI_IP27) || (defined(CONFIG_CPU_LOONGSON3) && defined(CONFIG_NUMA))

static void __init mips_bootmem_init(void)
{
	/* Reserve kernel code/data memory */
	mips_reserve_kernel_mem();

	/* Check and reserve memory occupied by initrd */
	mips_reserve_initrd_mem();

	/* Reserve memory for elfcorehdr */
	mips_reserve_elfcorehdr();

	/* Parse crashkernel parameter */
	mips_parse_crashkernel();

	/* Reserve memory for DMA contiguous allocator */
	dma_contiguous_reserve(mips_lowmem_limit);

	/* Allow memblock resize from now */
	memblock_allow_resize();
}

#else  /* !CONFIG_SGI_IP27 */

static void __init mips_bootmem_init(void)
{
	/* Reserve kernel code/data memory */
	mips_reserve_kernel_mem();

	/* Check and reserve memory occupied by initrd */
	mips_reserve_initrd_mem();

	/* Reserve memory for elfcorehdr */
	mips_reserve_elfcorehdr();

	/* Parse crashkernel parameter */
	mips_parse_crashkernel();

	/*
	 * Platform code usually copies fdt, but still lets reserve its memory
	 * in case if it doesn't
	 */
	early_init_fdt_reserve_self();

	/* Scan reserved-memory nodes of fdt */
	early_init_fdt_scan_reserved_mem();

	/* Reserve memory for DMA contiguous allocator */
	dma_contiguous_reserve(mips_lowmem_limit);

	/* Find memory PFN limits */
	find_pfn_limits();

	/* Allow memblock resize from now */
	memblock_allow_resize();
}

#endif	/* CONFIG_SGI_IP27 */

/*
 * arch_mem_init - initialize memory management subsystem
 *
 *  o plat_mem_setup() detects the memory configuration and will record detected
 *    memory areas using add_memory_region, which in addition preinitializes
 *    memblock ranges.
 *
 * At this stage the memory configuration of the system is known to the
 * kernel but generic memory management system is still entirely uninitialized.
 *
 *  o mips_parse_param() parses parameters passed to the kernel in accordance
 *    with CMDLINE configs.
 *  o sanity_check_meminfo() performs memory ranges sanity checks, for
 *    example, drop high mem regions if it's not supported, set memblock limit
 *    of low memory allocations
 *  o mips_bootmem_init() performs memblock further initializations,
 *    particularly reserve crucial regions, including kernel segments, initrd,
 *    elfcorehdrm, crashkernel, fdt, DMA contiguous allocator, set PFN-related
 *    global variables.
 *  o print_memory_map() prints initialized and verified memory map
 *  o device_tree_init() calls platform-specific method to perform some
 *    device tree related operations
 *  o plat_swiotlb_setup() - platform-specific SWIOTLB setup method
 *
 * Basic setup of page allocator is done in setup_arch():
 *  o paging_init() performs initialization of paging subsystem, in particular
 *    setup page tables (PGD, PMD, etc), kernel mapping, sparse memory segments
 *    if supported. It performs memory test if one is enabled. Finally it
 *    calculates memory zone limits and calls free_area_init_node()
 *    initializing pages memory maps, nodes, nodes free areas - basis of the
 *    buddy allocator.
 *
 * At this stage the bootmem allocator is ready to use.
 *
 * NOTE: historically plat_mem_setup did the entire platform initialization.
 *       This was rather impractical because it meant plat_mem_setup had to
 * get away without any kind of memory allocator.  To keep old code from
 * breaking plat_setup was just renamed to plat_mem_setup and a second platform
 * initialization hook for anything else was introduced.
 * Additionally boot_mem_map structure used to keep base memory layout so
 * then ancient bootmem allocator would be properly initialized. Since memblock
 * allocator is used for early memory management now, the boot_mem_map is
 * conserved just for compatibility.
 */
/*
 * MIPS early memory manager setup
 */
static void __init arch_mem_init(char **cmdline_p)
{
	/* call board setup routine */
	plat_mem_setup();

	/* Parse passed parameters */
	mips_parse_param(cmdline_p);

	/* Sanity check the specified memory */
	sanity_check_meminfo();

	/* Initialize memblock allocator */
	mips_bootmem_init();

	/* Print memory map initialized by arch-specific code and params */
	print_memory_map();

	/* Perform platform-specific device tree scanning */
	device_tree_init();

	/* Perform platform-specific SWIOTLB setup */
	plat_swiotlb_setup();
}

/*
 * Declare memory within system resources
 */
static void __init resource_init(void)
{
	struct memblock_region *reg;

	if (UNCAC_BASE != IO_BASE)
		return;

	/* Kernel code and data need to be registered within proper regions */
	code_resource.start = __pa_symbol(&_text);
	code_resource.end = __pa_symbol(&_etext) - 1;
	data_resource.start = __pa_symbol(&_etext);
	data_resource.end = __pa_symbol(&_edata) - 1;

	/* Register RAM resources */
	for_each_memblock(memory, reg) {
		struct resource *res;
		res = memblock_virt_alloc(sizeof(*res), 0);
		res->name  = "System RAM";
		res->start = PFN_PHYS(memblock_region_memory_base_pfn(reg));
		res->end = PFN_PHYS(memblock_region_memory_end_pfn(reg)) - 1;
		res->flags = IORESOURCE_BUSY | IORESOURCE_SYSTEM_RAM;

		request_resource(&iomem_resource, res);

		/*
		 *  We don't know which RAM region contains kernel data,
		 *  so we try it repeatedly and let the resource manager
		 *  test it.
		 */
		request_resource(res, &code_resource);
		request_resource(res, &data_resource);
		request_crashkernel(res);
	}
}

#ifdef CONFIG_SMP
static void __init prefill_possible_map(void)
{
	int i, possible = num_possible_cpus();

	if (possible > nr_cpu_ids)
		possible = nr_cpu_ids;

	for (i = 0; i < possible; i++)
		set_cpu_possible(i, true);
	for (; i < NR_CPUS; i++)
		set_cpu_possible(i, false);

	nr_cpu_ids = possible;
}
#else
static inline void prefill_possible_map(void) {}
#endif

void __init setup_arch(char **cmdline_p)
{
	cpu_probe();
	mips_cm_probe();
	prom_init();

	setup_early_fdc_console();
#ifdef CONFIG_EARLY_PRINTK
	setup_early_printk();
#endif
	cpu_report();
	check_bugs_early();

#if defined(CONFIG_VT)
#if defined(CONFIG_VGA_CONSOLE)
	conswitchp = &vga_con;
#elif defined(CONFIG_DUMMY_CONSOLE)
	conswitchp = &dummy_con;
#endif
#endif

	arch_mem_init(cmdline_p);

	resource_init();
	plat_smp_setup();
	prefill_possible_map();

	cpu_cache_init();
	paging_init();
}

unsigned long kernelsp[NR_CPUS];
unsigned long fw_arg0, fw_arg1, fw_arg2, fw_arg3;

#ifdef CONFIG_USE_OF
unsigned long fw_passed_dtb;
#endif

#ifdef CONFIG_DEBUG_FS
struct dentry *mips_debugfs_dir;
static int __init debugfs_mips(void)
{
	struct dentry *d;

	d = debugfs_create_dir("mips", NULL);
	if (!d)
		return -ENOMEM;
	mips_debugfs_dir = d;
	return 0;
}
arch_initcall(debugfs_mips);
#endif
