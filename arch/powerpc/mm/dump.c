/*
 * Copyright 2016, Rashmica Gupta, IBM Corp.
 * 
 * Debug helper to dump the current kernel pagetables of the system
 * so that we can see what the various memory ranges are set to.
 * 
 * Derived from the arm64 implementation:
 * Copyright (c) 2014, The Linux Foundation, Laura Abbott.
 * (C) Copyright 2008 Intel Corporation, Arjan van de Ven.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <asm/fixmap.h>
#include <asm/pgtable.h>
#include <linux/const.h>
#include <asm/page.h>

#define PUD_TYPE_MASK           (_AT(u64, 3) << 0)
#define PUD_TYPE_SECT           (_AT(u64, 1) << 0)
#define PMD_TYPE_MASK           (_AT(u64, 3) << 0)
#define PMD_TYPE_SECT           (_AT(u64, 1) << 0)

 
#if CONFIG_PGTABLE_LEVELS == 2
#include <asm-generic/pgtable-nopmd.h>
#elif CONFIG_PGTABLE_LEVELS == 3
#include <asm-generic/pgtable-nopud.h>
#endif
 
#define pmd_sect(pmd)  ((pmd_val(pmd) & PMD_TYPE_MASK) == PMD_TYPE_SECT)
#ifdef CONFIG_PPC_64K_PAGES
#define pud_sect(pud)           (0)
#else
#define pud_sect(pud)           ((pud_val(pud) & PUD_TYPE_MASK) == \
                                               PUD_TYPE_SECT)
#endif
	     

struct addr_marker {
	unsigned long start_address;
	const char *name;
};

enum address_markers_idx {
	VMALLOC_START_NR = 0,
	VMALLOC_END_NR,
	ISA_IO_START_NR,
	ISA_IO_END_NR,
	PHB_IO_START_NR,
	PHB_IO_END_NR,
	IOREMAP_START_NR,
	IOREMP_END_NR,
};

static struct addr_marker address_markers[] = {
	{ VMALLOC_START,	"vmalloc() Area" },
	{ VMALLOC_END,		"vmalloc() End" },
	{ ISA_IO_BASE,		"isa I/O start" },
	{ ISA_IO_END,		"isa I/O end" },
	{ PHB_IO_BASE,		"phb I/O start" },
	{ PHB_IO_END,		"phb I/O end" },
	{ IOREMAP_BASE,		"I/O remap start" },
	{ IOREMAP_END,		"I/O remap end" },
	{ -1,			NULL },
};

/*
 * The page dumper groups page table entries of the same type into a single
 * description. It uses pg_state to track the range information while
 * iterating over the pte entries. When the continuity is broken it then
 * dumps out a description of the range.
 */
struct pg_state {
	struct seq_file *seq;
	const struct addr_marker *marker;
	unsigned long start_address;
	unsigned level;
	u64 current_prot;
};

struct prot_bits {
	u64		mask;
	u64		val;
	const char	*set;
	const char	*clear;
};

static const struct prot_bits pte_bits[] = {
	{
		.mask	= _PAGE_USER,
		.val	= _PAGE_USER,
		.set	= "user",
		.clear	= "    ",
	}, {
		.mask	= _PAGE_RW,
		.val	= _PAGE_RW,
		.set	= "rw",
		.clear	= "ro",
	}, {
		.mask	= _PAGE_EXEC,
		.val	= _PAGE_EXEC,
		.set	= " X ",
		.clear	= "   ",
	}, {
		.mask	= _PAGE_PTE,
		.val	= _PAGE_PTE,
		.set	= "pte",
		.clear	= "   ",
	}, {
		.mask	= _PAGE_PRESENT,
		.val	= _PAGE_PRESENT,
		.set	= "present",
		.clear	= "       ",
	}, {
		.mask	= _PAGE_HASHPTE,
		.val	= _PAGE_HASHPTE,
		.set	= "htpe",
		.clear	= "    ",
	}, {
		.mask	= _PAGE_GUARDED,
		.val	= _PAGE_GUARDED,
		.set	= "guarded",
		.clear	= "       ",
	}, {
		.mask	= _PAGE_DIRTY,
		.val	= _PAGE_DIRTY,
		.set	= "dirty",
		.clear	= "     ",
	}, {
		.mask	= _PAGE_ACCESSED,
		.val	= _PAGE_ACCESSED,
		.set	= "accessed",
		.clear	= "        ",
	}, {
		.mask	= _PAGE_WRITETHRU,
		.val	= _PAGE_WRITETHRU,
		.set	= "write through",
		.clear	= "             ",
	}, {
		.mask	= _PAGE_NO_CACHE,
		.val	= _PAGE_NO_CACHE,
		.set	= "no cache",
		.clear	= "        ",
	}, {
		.mask	= _PAGE_BUSY,
		.val	= _PAGE_BUSY,
		.set	= "busy",
	}, {
		.mask	= _PAGE_F_GIX,
		.val	= _PAGE_F_GIX,
		.set	= "gix",
	}, {
		.mask	= _PAGE_F_SECOND,
		.val	= _PAGE_F_SECOND,
		.set	= "second",
	}, {
		.mask	= _PAGE_SPECIAL,
		.val	= _PAGE_SPECIAL,
		.set	= "special",
	}
};

struct pg_level {
	const struct prot_bits *bits;
	size_t num;
	u64 mask;
};

static struct pg_level pg_level[] = {
	{
	}, { /* pgd */
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	}, { /* pud */
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	}, { /* pmd */
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	}, { /* pte */
		.bits	= pte_bits,
		.num	= ARRAY_SIZE(pte_bits),
	},
};

static void dump_prot(struct pg_state *st, const struct prot_bits *bits,
			size_t num)
{
	unsigned i;

	for (i = 0; i < num; i++, bits++) {
		const char *s;

		if ((st->current_prot & bits->mask) == bits->val)
			s = bits->set;
		else
			s = bits->clear;

		if (s)
			seq_printf(st->seq, " %s", s);
	}
}

static void note_page(struct pg_state *st, unsigned long addr, unsigned level,
				u64 val)
{
	static const char units[] = "KMGTPE";
	u64 prot = val & pg_level[level].mask;

	/* At first no level is set */
	if (!st->level) {
		st->level = level;
		st->current_prot = prot;
		st->start_address = addr;
		seq_printf(st->seq, "---[ %s ]---\n", st->marker->name);
	/* We are only interested in dumping when something (protection,
	 *  level of PTE or the section of vmalloc) has changed */
	} else if (prot != st->current_prot || level != st->level ||
		   addr >= st->marker[1].start_address) {
		const char *unit = units;
		unsigned long delta;

		/* Check protection on PTE */
		if (st->current_prot) {
			seq_printf(st->seq, "0x%016lx-0x%016lx   ",
				   st->start_address, addr-1);

			delta = (addr - st->start_address) >> 10;
			/* Work out what appropriate unit to use */
			while (!(delta & 1023) && unit[1]) {
				delta >>= 10;
				unit++;
			}
			seq_printf(st->seq, "%9lu%c", delta, *unit);
			/* Dump all the protection flags */
			if (pg_level[st->level].bits)
				dump_prot(st, pg_level[st->level].bits,
					  pg_level[st->level].num);
			seq_puts(st->seq, "\n");
		}
		/* Address indicates we have passed the end of the
		 * current section of vmalloc */
		while (addr >= st->marker[1].start_address) {
			st->marker++;
			seq_printf(st->seq, "---[ %s ]---\n", st->marker->name);
		}

		st->start_address = addr;
		st->current_prot = prot;
		st->level = level;
	}

}

static void walk_pte(struct pg_state *st, pmd_t *pmd, unsigned long start)
{
	pte_t *pte = pte_offset_kernel(pmd, 0);
	unsigned long addr;
	unsigned i;

	for (i = 0; i < PTRS_PER_PTE; i++, pte++) {
		addr = start + i * PAGE_SIZE;
		note_page(st, addr, 4, pte_val(*pte));
	}
}

static void walk_pmd(struct pg_state *st, pud_t *pud, unsigned long start)
{
	pmd_t *pmd = pmd_offset(pud, 0);
	unsigned long addr;
	unsigned i;

	for (i = 0; i < PTRS_PER_PMD; i++, pmd++) {
		addr = start + i * PMD_SIZE;
		if (!pmd_none(*pmd) && !pmd_sect(*pmd))
			/* pmd exists */
			walk_pte(st, pmd, addr);
		else
			note_page(st, addr, 3, pmd_val(*pmd));
	}
}

static void walk_pud(struct pg_state *st, pgd_t *pgd, unsigned long start)
{
	pud_t *pud = pud_offset(pgd, 0);
	unsigned long addr = 0UL;
	unsigned i;

	for (i = 0; i < PTRS_PER_PUD; i++, pud++) {
		addr = start + i * PUD_SIZE;
		if (!pud_none(*pud) && !pud_sect(*pud))
			/* pud exists */
			walk_pmd(st, pud, addr);
		else
			note_page(st, addr, 2, pud_val(*pud));
	}
}

static void walk_pgd(struct pg_state *st, unsigned long start)
{
	pgd_t *pgd = pgd_offset_k( 0UL);
	unsigned i;
	unsigned long addr;

	for (i = 0; i < PTRS_PER_PGD; i++, pgd++) {
		addr = start + i * PGDIR_SIZE;
		if(!pgd_none(*pgd))
			/* pgd exists */
			walk_pud(st, pgd, addr);
		else
			note_page(st, addr, 1, pgd_val(*pgd));

	}
}

static int ptdump_show(struct seq_file *m, void *v)
{
	struct pg_state st = {
		.seq = m,
		.start_address = KERN_VIRT_START,
		.marker = address_markers,
	};
	/* Traverse kernel page tables */
	walk_pgd(&st, KERN_VIRT_START);
	note_page(&st, 0, 0, 0);
	return 0;
}

static int ptdump_open(struct inode *inode, struct file *file)
{
	return single_open(file, ptdump_show, NULL);
}

static const struct file_operations ptdump_fops = {
	.open		= ptdump_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int ptdump_init(void)
{
	struct dentry *pe;
	unsigned i, j;

	for (i = 0; i < ARRAY_SIZE(pg_level); i++)
		if (pg_level[i].bits)
			for (j = 0; j < pg_level[i].num; j++)
				pg_level[i].mask |= pg_level[i].bits[j].mask;

	pe = debugfs_create_file("kernel_page_tables", 0400, NULL, NULL,
				 &ptdump_fops);
	return pe ? 0 : -ENOMEM;
}
device_initcall(ptdump_init);
