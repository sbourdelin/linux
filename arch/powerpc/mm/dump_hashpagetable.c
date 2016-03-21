/*
 * Copyright 2016, Rashmica Gupta, IBM Corp.
 *
 * This traverses the kernel virtual memory and dumps the pages that are in
 * the hash pagetable, along with their flags to
 * /sys/kernel/debug/kernel_hash_pagetable.
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
#include <asm/pgalloc.h>
#include <asm/plpar_wrappers.h>
#include <linux/memblock.h>
#include <asm/firmware.h>

struct addr_marker {
	unsigned long start_address;
	const char *name;
};

static struct addr_marker address_markers[] = {
	{ PAGE_OFFSET,		"Start of kernel VM"},
	{ VMALLOC_START,	"vmalloc() Area" },
	{ VMALLOC_END,		"vmalloc() End" },
	{ ISA_IO_BASE,		"isa I/O start" },
	{ ISA_IO_END,		"isa I/O end" },
	{ PHB_IO_BASE,		"phb I/O start" },
	{ PHB_IO_END,		"phb I/O end" },
	{ IOREMAP_BASE,		"I/O remap start" },
	{ IOREMAP_END,		"I/O remap end" },
	{ VMEMMAP_BASE,		"vmemmap start" },
	{ -1,			NULL },
};

struct pg_state {
	struct seq_file *seq;
	const struct addr_marker *marker;
	unsigned long start_address;
	unsigned level;
	u64 current_flags;
};

struct flag_info {
	u64		mask;
	u64		val;
	const char	*set;
	const char	*clear;
	bool		is_val;
};

static const struct flag_info v_flag_array[] = {
	{
		.mask   = SLB_VSID_B,
		.val    = SLB_VSID_B_256M,
		.set    = "ssize: 256M",
		.clear  = "ssize: 1T  ",
	}, {
		.mask	= HPTE_V_SECONDARY,
		.val	= HPTE_V_SECONDARY,
		.set	= "secondary",
		.clear	= "primary  ",
	}, {
		.mask	= HPTE_V_VALID,
		.val	= HPTE_V_VALID,
		.set	= "valid  ",
		.clear	= "invalid",
	}, {
		.mask	= HPTE_V_BOLTED,
		.val	= HPTE_V_BOLTED,
		.set	= "bolted",
		.clear	= "",
	}
};

static const struct flag_info r_flag_array[] = {
	{
		.mask	= HPTE_R_PP0 | HPTE_R_PP,
		.val	= HPTE_R_PP0 | HPTE_R_PP,
		.set	= "prot",
		.clear	= "",
		.is_val = true,
	}, {
		.mask	= HPTE_R_KEY_HI | HPTE_R_KEY_LO,
		.val	= HPTE_R_KEY_HI | HPTE_R_KEY_LO,
		.set	= "key",
		.clear	= "",
		.is_val = true,
	}, {
		.mask	= HPTE_R_R,
		.val	= HPTE_R_R,
		.set	= "ref",
		.clear	= "   ",
	}, {
		.mask	= HPTE_R_C,
		.val	= HPTE_R_C,
		.set	= "changed",
		.clear	= "       ",
	}, {
		.mask	= HPTE_R_N,
		.val	= HPTE_R_N,
		.set	= "no execute",
		.clear	= "",
	}, {
		.mask	= HPTE_R_WIMG,
		.val	= HPTE_R_W,
		.set	= "writethru",
		.clear	= "",
	}, {
		.mask	= HPTE_R_WIMG,
		.val	= HPTE_R_I,
		.set	= "no cache",
		.clear	= "",
	}, {
		.mask	= HPTE_R_WIMG,
		.val	= HPTE_R_G,
		.set	= "guarded",
		.clear	= "",
	}
};

static void dump_flag_info(struct pg_state *st, const struct flag_info
		*flag, unsigned long pte, int num)
{
	unsigned i;

	for (i = 0; i < num; i++, flag++) {
		const char *s = NULL;

		if (flag->is_val) {
			seq_printf(st->seq, "  %s:%llx", flag->set, pte &
					flag->val);
		} else {
			if ((pte & flag->mask) == flag->val)
				s = flag->set;
			else
				s = flag->clear;
			seq_printf(st->seq, "  %s", s);
		}
	}
}

static void dump_hpte_info(struct pg_state *st, unsigned long ea, unsigned long
		v, unsigned long r, unsigned long rpn, int bps, int aps,
		unsigned long lp)
{
	static const char units[] = "BKMGTPE";
	const char *unit = units;

	while (ea >= st->marker[1].start_address) {
		st->marker++;
		seq_printf(st->seq, "---[ %s ]---\n", st->marker->name);
	}
	seq_printf(st->seq, "0x%lx:\t", ea);
	seq_printf(st->seq, "AVPN:%lx\t", HPTE_V_AVPN_VAL(v));
	dump_flag_info(st, v_flag_array, v, ARRAY_SIZE(v_flag_array));
	seq_printf(st->seq, "  rpn: %lx\t", rpn);
	dump_flag_info(st, r_flag_array, r, ARRAY_SIZE(r_flag_array));

	while (bps > 9 && unit[1]) {
		bps -= 10;
		unit++;
	}
	seq_printf(st->seq, "base_ps: %i%c\t", 1<<bps, *unit);
	while (aps > 9 && unit[1]) {
		aps -= 10;
		unit++;
	}
	seq_printf(st->seq, "actual_ps: %i%c", 1<<aps, *unit);
	if (lp != -1)
		seq_printf(st->seq, "\tLP enc: %lx", lp);
	seq_puts(st->seq, "\n");
}

static int native_find(unsigned long ea, int psize, bool primary, unsigned long
		*v, unsigned long *r)
{
	struct hash_pte *hptep;
	unsigned long hash, vsid, vpn, hpte_group, want_v, hpte_v;
	int i, ssize = mmu_kernel_ssize;
	unsigned long shift = mmu_psize_defs[psize].shift;

	/* calculate hash */
	vsid = get_kernel_vsid(ea, ssize);
	vpn  = hpt_vpn(ea, vsid, ssize);
	hash = hpt_hash(vpn, shift, ssize);
	want_v = hpte_encode_avpn(vpn, psize, ssize);

	/* to check in the secondary hash table, we invert the hash */
	if (!primary)
		hash = ~hash;
	hpte_group = (hash & htab_hash_mask) * HPTES_PER_GROUP;
	for (i = 0; i < HPTES_PER_GROUP; i++) {
		hptep = htab_address + hpte_group;
		hpte_v = be64_to_cpu(hptep->v);

		if (HPTE_V_COMPARE(hpte_v, want_v) && (hpte_v & HPTE_V_VALID)) {
			/* HPTE matches */
			*v = be64_to_cpu(hptep->v);
			*r = be64_to_cpu(hptep->r);
			return 0;
		}
		++hpte_group;
	}
	return -1;
}

static int pseries_find(unsigned long ea, int psize, bool primary, unsigned
		long *v, unsigned long *r)
{
	struct hash_pte ptes[4];
	unsigned long vsid, vpn, hash, hpte_group, want_v;
	int i, j, ssize = mmu_kernel_ssize;
	long lpar_rc = 0;
	unsigned long shift = mmu_psize_defs[psize].shift;

	/* calculate hash */
	vsid = get_kernel_vsid(ea, ssize);
	vpn  = hpt_vpn(ea, vsid, ssize);
	hash = hpt_hash(vpn, shift, ssize);
	want_v = hpte_encode_avpn(vpn, psize, ssize);

	/* to check in the secondary hash table, we invert the hash */
	if (!primary)
		hash = ~hash;
	hpte_group = ((hash & htab_hash_mask) * HPTES_PER_GROUP) & ~0x7UL;
	/* see if we can find an entry in the hpte with this hash */
	for (i = 0; i < HPTES_PER_GROUP; i += 4, hpte_group += 4) {
		lpar_rc = plpar_pte_read_4(0, hpte_group, (void *)ptes);

		if (lpar_rc != H_SUCCESS)
			continue;
		for (j = 0; j < 4; j++) {
			if (HPTE_V_COMPARE(ptes[j].v, want_v) &&
					(ptes[j].v & HPTE_V_VALID)) {
				/* HPTE matches */
				*v = ptes[j].v;
				*r = ptes[j].r;
				return 0;
			}
		}
	}
	return -1;
}

static void decode_r(int bps, unsigned long r, unsigned long *rpn, int *aps,
		unsigned long *lp_bits)
{
	struct mmu_psize_def entry;
	unsigned long arpn, mask, lp;
	int penc = -2, idx = 0, shift;

	/*.
	 * The LP field has 8 bits. Depending on the actual page size, some of
	 * these bits are concatenated with the APRN to get the RPN. The rest
	 * of the bits in the LP field is the LP value and is an encoding for
	 * the base page size and the actual page size.
	 *
	 *  -	find the mmu entry for our base page size
	 *  -	go through all page encodings and use the associated mask to
	 *	find an encoding that matches our encoding in the LP field.
	 */
	arpn = (r & HPTE_R_RPN) >> HPTE_R_RPN_SHIFT;
	lp = arpn & 0xff;

	entry = mmu_psize_defs[bps];
	while (idx < MMU_PAGE_COUNT) {
		penc = entry.penc[idx];
		if ((penc != -1) && (mmu_psize_defs[idx].shift)) {
			shift = mmu_psize_defs[idx].shift -  HPTE_R_RPN_SHIFT;
			mask = (0x1 << (shift)) - 1;
			if ((lp & mask) == penc) {
				*aps = mmu_psize_to_shift(idx);
				*lp_bits = lp & mask;
				*rpn = arpn >> shift;
				return;
			}
		}
		idx++;
	}
}

static unsigned long hpte_find(struct pg_state *st, unsigned long ea, int psize)
{
	unsigned long slot;
	unsigned long v  = 0, r = 0, rpn, lp_bits;
	int base_psize = 0, actual_psize = 0;

	if (ea <= PAGE_OFFSET)
		return -1;

	/* Look in primary table */
	if (firmware_has_feature(FW_FEATURE_LPAR))
		slot = pseries_find(ea, psize, true, &v, &r);
	else
		slot = native_find(ea, psize, true, &v, &r);

	/* Look in secondary table */
	if (slot == -1) {
		if (firmware_has_feature(FW_FEATURE_LPAR))
			slot = pseries_find(ea, psize, false, &v, &r);
		else
			slot = native_find(ea, psize, false, &v, &r);
	}

	/* No entry found */
	if (slot == -1)
		return -1;

	/* We found an entry in the hash page table:
	 *  - check that this has the same base page
	 *  - find the actual page size
	 *  - find the RPN
	 */
	base_psize = mmu_psize_to_shift(psize);

	if ((v & HPTE_V_LARGE) == HPTE_V_LARGE) {
		decode_r(psize, r, &rpn, &actual_psize, &lp_bits);
	} else {
		/* 4K actual page size */
		actual_psize = 12;
		rpn = (r & HPTE_R_RPN) >> HPTE_R_RPN_SHIFT;
		/* In this case there are no LP bits */
		lp_bits = -1;
	}
	/* We didn't find a matching encoding, so the PTE we found isn't for
	 * this address.
	 */
	if (actual_psize == -1)
		return -1;

	dump_hpte_info(st, ea, v, r, rpn, base_psize, actual_psize, lp_bits);
	return 0;
}

static void walk_pte(struct pg_state *st, pmd_t *pmd, unsigned long start)
{
	pte_t *pte = pte_offset_kernel(pmd, 0);
	unsigned long addr, pteval, psize;
	int i, status;

	for (i = 0; i < PTRS_PER_PTE; i++, pte++) {
		addr = start + i * PAGE_SIZE;
		pteval = pte_val(*pte);

		if (addr < VMALLOC_END)
			psize = mmu_vmalloc_psize;
		else
			psize = mmu_io_psize;

		/* check for secret 4K mappings */
		if (((pteval & _PAGE_COMBO) == _PAGE_COMBO) ||
			((pteval & _PAGE_4K_PFN) == _PAGE_4K_PFN))
			psize = mmu_io_psize;

		/* check for hashpte */
		status = hpte_find(st, addr, psize);

		if (((pteval & _PAGE_HASHPTE) != _PAGE_HASHPTE)
				&& (status != -1)) {
		/* found a hpte that is not in the linux page tables */
			seq_printf(st->seq, "page probably bolted before linux"
				" pagetables were set: addr:%lx, pteval:%lx\n",
				addr, pteval);
		}
	}
}

static void walk_pmd(struct pg_state *st, pud_t *pud, unsigned long start)
{
	pmd_t *pmd = pmd_offset(pud, 0);
	unsigned long addr;
	unsigned i;

	for (i = 0; i < PTRS_PER_PMD; i++, pmd++) {
		addr = start + i * PMD_SIZE;
		if (!pmd_none(*pmd))
			/* pmd exists */
			walk_pte(st, pmd, addr);
	}
}

static void walk_pud(struct pg_state *st, pgd_t *pgd, unsigned long start)
{
	pud_t *pud = pud_offset(pgd, 0);
	unsigned long addr;
	unsigned i;

	for (i = 0; i < PTRS_PER_PUD; i++, pud++) {
		addr = start + i * PUD_SIZE;
		if (!pud_none(*pud))
			/* pud exists */
			walk_pmd(st, pud, addr);
	}
}

static void walk_linearmapping(struct pg_state *st)
{
	unsigned long addr;

	/* Traverse the linear mapping section of virtual memory and dump pages
	 * that are in the hash pagetable.
	 */
	for (addr = PAGE_OFFSET; addr < PAGE_OFFSET +
			memblock_phys_mem_size(); addr += PAGE_SIZE)
		hpte_find(st, addr, mmu_linear_psize);
}

static void walk_pagetables(struct pg_state *st)
{
	pgd_t *pgd = pgd_offset_k(0UL);
	unsigned i;
	unsigned long addr;

	/* Traverse the linux pagetable structure and dump pages that are in
	 * the hash pagetable.
	 */
	for (i = 0; i < PTRS_PER_PGD; i++, pgd++) {
		addr = VMALLOC_START + i * PGDIR_SIZE;
		if (!pgd_none(*pgd))
			/* pgd exists */
			walk_pud(st, pgd, addr);
	}
}

static void walk_vmemmap(struct pg_state *st)
{
	struct vmemmap_backing *ptr = vmemmap_list;

	/* Traverse the vmemmaped memory and dump pages that are in the hash
	 * pagetable.
	 */
	while (ptr->list) {
		hpte_find(st, ptr->virt_addr, mmu_vmemmap_psize);
		ptr = ptr->list;
	}
	seq_puts(st->seq, "---[ vmemmap end ]---\n");
}

static int ptdump_show(struct seq_file *m, void *v)
{
	struct pg_state st = {
		.seq = m,
		.start_address = PAGE_OFFSET,
		.marker = address_markers,
	};
	/* Traverse the 0xc, 0xd and 0xf areas of the kernel virtual memory and
	 * dump pages that are in the hash pagetable.
	 */
	walk_linearmapping(&st);
	walk_pagetables(&st);
	walk_vmemmap(&st);
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
	struct dentry *debugfs_file;

	debugfs_file = debugfs_create_file("kernel_hash_pagetable", 0400,
			NULL, NULL, &ptdump_fops);
	return debugfs_file ? 0 : -ENOMEM;
}
device_initcall(ptdump_init);
