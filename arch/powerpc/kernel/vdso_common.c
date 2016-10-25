#ifndef BITS
#define BITS 32
#endif

#undef Elf_Ehdr
#undef Elf_Sym
#undef Elf_Shdr

#define _CONCAT3(a, b, c)	a ## b ## c
#define CONCAT3(a, b, c)	_CONCAT3(a, b, c)
#define Elf_Ehdr	CONCAT3(Elf,  BITS, _Ehdr)
#define Elf_Sym		CONCAT3(Elf,  BITS, _Sym)
#define Elf_Shdr	CONCAT3(Elf,  BITS, _Shdr)
#define VDSO_LBASE	CONCAT3(VDSO, BITS, _LBASE)
#define vdso_kbase	CONCAT3(vdso, BITS, _kbase)
#define vdso_pages	CONCAT3(vdso, BITS, _pages)
#define vdso_pagelist	CONCAT3(vdso, BITS, _pagelist)

#undef pr_fmt
#define pr_fmt(fmt)	"vDSO" __stringify(BITS) ": " fmt

#define lib_elfinfo CONCAT3(lib, BITS, _elfinfo)

#define find_section CONCAT3(find_section, BITS,)
static void * __init find_section(Elf_Ehdr *ehdr, const char *secname,
		unsigned long *size)
{
	Elf_Shdr *sechdrs;
	unsigned int i;
	char *secnames;

	/* Grab section headers and strings so we can tell who is who */
	sechdrs = (void *)ehdr + ehdr->e_shoff;
	secnames = (void *)ehdr + sechdrs[ehdr->e_shstrndx].sh_offset;

	/* Find the section they want */
	for (i = 1; i < ehdr->e_shnum; i++) {
		if (strcmp(secnames+sechdrs[i].sh_name, secname) == 0) {
			if (size)
				*size = sechdrs[i].sh_size;
			return (void *)ehdr + sechdrs[i].sh_offset;
		}
	}
	if (size)
		*size = 0;
	return NULL;
}

#define find_symbol CONCAT3(find_symbol, BITS,)
static Elf_Sym * __init find_symbol(struct lib_elfinfo *lib,
		const char *symname)
{
	unsigned int i;
	char name[MAX_SYMNAME], *c;

	for (i = 0; i < (lib->dynsymsize / sizeof(Elf_Sym)); i++) {
		if (lib->dynsym[i].st_name == 0)
			continue;
		strlcpy(name, lib->dynstr + lib->dynsym[i].st_name,
			MAX_SYMNAME);
		c = strchr(name, '@');
		if (c)
			*c = 0;
		if (strcmp(symname, name) == 0)
			return &lib->dynsym[i];
	}
	return NULL;
}

/*
 * Note that we assume the section is .text and the symbol is relative to
 * the library base.
 */
#define find_function CONCAT3(find_function, BITS,)
static unsigned long __init find_function(struct lib_elfinfo *lib,
		const char *symname)
{
	Elf_Sym *sym = find_symbol(lib, symname);

	if (sym == NULL) {
		pr_warn("function %s not found !\n", symname);
		return 0;
	}
#if defined(VDS64_HAS_DESCRIPTORS) && (BITS == 64)
	return *((u64 *)(vdso64_kbase + sym->st_value - VDSO64_LBASE)) -
		VDSO64_LBASE;
#else
	return sym->st_value - VDSO_LBASE;
#endif
}

#define vdso_do_func_patch CONCAT3(vdso_do_func_patch, BITS,)
static int __init vdso_do_func_patch(struct lib_elfinfo *v,
		const char *orig, const char *fix)
{
	Elf_Sym *sym_gen, *sym_fix;

	sym_gen = find_symbol(v, orig);
	if (sym_gen == NULL) {
		pr_err("Can't find symbol %s !\n", orig);
		return -1;
	}
	if (fix == NULL) {
		sym_gen->st_name = 0;
		return 0;
	}
	sym_fix = find_symbol(v, fix);
	if (sym_fix == NULL) {
		pr_err("Can't find symbol %s !\n", fix);
		return -1;
	}
	sym_gen->st_value = sym_fix->st_value;
	sym_gen->st_size = sym_fix->st_size;
	sym_gen->st_info = sym_fix->st_info;
	sym_gen->st_other = sym_fix->st_other;
	sym_gen->st_shndx = sym_fix->st_shndx;

	return 0;
}

#define vdso_do_find_sections CONCAT3(vdso_do_find_sections, BITS,)
static __init int vdso_do_find_sections(struct lib_elfinfo *v)
{
	void *sect;

	/*
	 * Locate symbol tables & text section
	 */
	v->dynsym = find_section(v->hdr, ".dynsym", &v->dynsymsize);
	v->dynstr = find_section(v->hdr, ".dynstr", NULL);
	if (v->dynsym == NULL || v->dynstr == NULL) {
		pr_err("required symbol section not found\n");
		return -1;
	}

	sect = find_section(v->hdr, ".text", NULL);
	if (sect == NULL) {
		pr_err("the .text section was not found\n");
		return -1;
	}
	v->text = sect - vdso_kbase;

	return 0;
}

#define vdso_fixup_datapage CONCAT3(vdso_fixup_datapage, BITS,)
static __init int vdso_fixup_datapage(struct lib_elfinfo *v)
{
	Elf_Sym *sym = find_symbol(v, "__kernel_datapage_offset");

	if (sym == NULL) {
		pr_err("Can't find symbol __kernel_datapage_offset !\n");
		return -1;
	}
	*((int *)(vdso_kbase + sym->st_value - VDSO_LBASE)) =
		(vdso_pages << PAGE_SHIFT) - (sym->st_value - VDSO_LBASE);

	return 0;
}

#define vdso_fixup_features CONCAT3(vdso_fixup_features, BITS,)
static __init int vdso_fixup_features(struct lib_elfinfo *v)
{
	unsigned long size;
	void *start;

	start = find_section(v->hdr, "__ftr_fixup", &size);
	if (start)
		do_feature_fixups(cur_cpu_spec->cpu_features,
				  start, start + size);

	start = find_section(v->hdr, "__mmu_ftr_fixup", &size);
	if (start)
		do_feature_fixups(cur_cpu_spec->mmu_features,
				  start, start + size);

#ifdef CONFIG_PPC64
	start = find_section(v->hdr, "__fw_ftr_fixup", &size);
	if (start)
		do_feature_fixups(powerpc_firmware_features,
				  start, start + size);
#endif /* CONFIG_PPC64 */

	start = find_section(v->hdr, "__lwsync_fixup", &size);
	if (start)
		do_lwsync_fixups(cur_cpu_spec->cpu_features,
				 start, start + size);

	return 0;
}

#define vdso_setup CONCAT3(vdso_setup, BITS,)
static __init int vdso_setup(struct lib_elfinfo *v)
{
	v->hdr = vdso_kbase;

	if (vdso_do_find_sections(v))
		return -1;
	if (vdso_fixup_datapage(v))
		return -1;
	if (vdso_fixup_features(v))
		return -1;
	return 0;
}

#define init_vdso_pagelist CONCAT3(init_vdso, BITS, _pagelist)
static __init void init_vdso_pagelist(void)
{
	int i;

	/* Make sure pages are in the correct state */
	vdso_pagelist = kzalloc(sizeof(struct page *) * (vdso_pages + 2),
				  GFP_KERNEL);
	BUG_ON(vdso_pagelist == NULL);
	for (i = 0; i < vdso_pages; i++) {
		struct page *pg = virt_to_page(vdso_kbase + i*PAGE_SIZE);

		ClearPageReserved(pg);
		get_page(pg);
		vdso_pagelist[i] = pg;
	}
	vdso_pagelist[i++] = virt_to_page(vdso_data);
	vdso_pagelist[i] = NULL;
}

#undef find_section
#undef find_symbol
#undef find_function
#undef vdso_do_func_patch
#undef vdso_do_find_sections
#undef vdso_fixup_datapage
#undef vdso_fixup_features
#undef vdso_setup
#undef init_vdso_pagelist

#undef VDSO_LBASE
#undef vdso_kbase
#undef vdso_pages
#undef vdso_pagelist
#undef lib_elfinfo
#undef BITS
#undef _CONCAT3
#undef CONCAT3
