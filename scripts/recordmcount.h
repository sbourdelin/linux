/*
 * recordmcount.h
 *
 * This code was taken out of recordmcount.c written by
 * Copyright 2009 John F. Reiser <jreiser@BitWagon.com>.  All rights reserved.
 *
 * The original code had the same algorithms for both 32bit
 * and 64bit ELF files, but the code was duplicated to support
 * the difference in structures that were used. This
 * file creates a macro of everything that is different between
 * the 64 and 32 bit code, such that by including this header
 * twice we can create both sets of functions by including this
 * header once with RECORD_MCOUNT_64 undefined, and again with
 * it defined.
 *
 * This conversion to macros was done by:
 * Copyright 2010 Steven Rostedt <srostedt@redhat.com>, Red Hat Inc.
 *
 * Licensed under the GNU General Public License, version 2 (GPLv2).
 */
#undef append_section
#undef append_func
#undef is_fake_mcount
#undef fn_is_fake_mcount
#undef MIPS_is_fake_mcount
#undef mcount_adjust
#undef add_relocation
#undef sift_rel_mcount
#undef nop_mcount
#undef find_secsym_ndx
#undef __has_rel_mcount
#undef has_rel_mcount
#undef tot_relsize
#undef get_mcountsym
#undef get_arm_sym
#undef get_sym_str_and_relp
#undef do_func
#undef Elf_Addr
#undef Elf_Ehdr
#undef Elf_Shdr
#undef Elf_Rel
#undef Elf_Rela
#undef Elf_Sym
#undef ELF_R_SYM
#undef Elf_r_sym
#undef ELF_R_INFO
#undef Elf_r_info
#undef ELF_ST_BIND
#undef ELF_ST_TYPE
#undef fn_ELF_R_SYM
#undef fn_ELF_R_INFO
#undef uint_t
#undef _w
#undef _align
#undef _size

#ifdef RECORD_MCOUNT_64
# define append_section		append_section64
# define append_func		append64
# define add_relocation		add_relocation_64
# define sift_rel_mcount	sift64_rel_mcount
# define nop_mcount		nop_mcount_64
# define find_secsym_ndx	find64_secsym_ndx
# define __has_rel_mcount	__has64_rel_mcount
# define has_rel_mcount		has64_rel_mcount
# define tot_relsize		tot64_relsize
# define get_sym_str_and_relp	get_sym_str_and_relp_64
# define do_func		do64
# define get_mcountsym		get_mcountsym_64
# define get_arm_sym		get_arm_sym_64
# define is_fake_mcount		is_fake_mcount64
# define fn_is_fake_mcount	fn_is_fake_mcount64
# define MIPS_is_fake_mcount	MIPS64_is_fake_mcount
# define mcount_adjust		mcount_adjust_64
# define Elf_Addr		Elf64_Addr
# define Elf_Ehdr		Elf64_Ehdr
# define Elf_Shdr		Elf64_Shdr
# define Elf_Rel		Elf64_Rel
# define Elf_Rela		Elf64_Rela
# define Elf_Sym		Elf64_Sym
# define ELF_R_SYM		ELF64_R_SYM
# define Elf_r_sym		Elf64_r_sym
# define ELF_R_INFO		ELF64_R_INFO
# define Elf_r_info		Elf64_r_info
# define ELF_ST_BIND		ELF64_ST_BIND
# define ELF_ST_TYPE		ELF64_ST_TYPE
# define fn_ELF_R_SYM		fn_ELF64_R_SYM
# define fn_ELF_R_INFO		fn_ELF64_R_INFO
# define uint_t			uint64_t
# define _w			w8
# define _align			7u
# define _size			8
#else
# define append_section		append_section32
# define append_func		append32
# define add_relocation		add_relocation_32
# define sift_rel_mcount	sift32_rel_mcount
# define nop_mcount		nop_mcount_32
# define find_secsym_ndx	find32_secsym_ndx
# define __has_rel_mcount	__has32_rel_mcount
# define has_rel_mcount		has32_rel_mcount
# define tot_relsize		tot32_relsize
# define get_sym_str_and_relp	get_sym_str_and_relp_32
# define do_func		do32
# define get_mcountsym		get_mcountsym_32
# define get_arm_sym		get_arm_sym_32
# define is_fake_mcount		is_fake_mcount32
# define fn_is_fake_mcount	fn_is_fake_mcount32
# define MIPS_is_fake_mcount	MIPS32_is_fake_mcount
# define mcount_adjust		mcount_adjust_32
# define Elf_Addr		Elf32_Addr
# define Elf_Ehdr		Elf32_Ehdr
# define Elf_Shdr		Elf32_Shdr
# define Elf_Rel		Elf32_Rel
# define Elf_Rela		Elf32_Rela
# define Elf_Sym		Elf32_Sym
# define ELF_R_SYM		ELF32_R_SYM
# define Elf_r_sym		Elf32_r_sym
# define ELF_R_INFO		ELF32_R_INFO
# define Elf_r_info		Elf32_r_info
# define ELF_ST_BIND		ELF32_ST_BIND
# define ELF_ST_TYPE		ELF32_ST_TYPE
# define fn_ELF_R_SYM		fn_ELF32_R_SYM
# define fn_ELF_R_INFO		fn_ELF32_R_INFO
# define uint_t			uint32_t
# define _w			w
# define _align			3u
# define _size			4
#endif

/* Functions and pointers that do_file() may override for specific e_machine. */
static int fn_is_fake_mcount(Elf_Rel const *rp)
{
	return 0;
}
static int (*is_fake_mcount)(Elf_Rel const *rp) = fn_is_fake_mcount;

static uint_t fn_ELF_R_SYM(Elf_Rel const *rp)
{
	return ELF_R_SYM(_w(rp->r_info));
}
static uint_t (*Elf_r_sym)(Elf_Rel const *rp) = fn_ELF_R_SYM;

static void fn_ELF_R_INFO(Elf_Rel *const rp, unsigned sym, unsigned type)
{
	rp->r_info = _w(ELF_R_INFO(sym, type));
}
static void (*Elf_r_info)(Elf_Rel *const rp, unsigned sym, unsigned type) = fn_ELF_R_INFO;

static int mcount_adjust = 0;

/*
 * MIPS mcount long call has 2 _mcount symbols, only the position of the 1st
 * _mcount symbol is needed for dynamic function tracer, with it, to disable
 * tracing(ftrace_make_nop), the instruction in the position is replaced with
 * the "b label" instruction, to enable tracing(ftrace_make_call), replace the
 * instruction back. So, here, we set the 2nd one as fake and filter it.
 *
 * c:	3c030000	lui	v1,0x0		<-->	b	label
 *		c: R_MIPS_HI16	_mcount
 *		c: R_MIPS_NONE	*ABS*
 *		c: R_MIPS_NONE	*ABS*
 * 10:	64630000	daddiu	v1,v1,0
 *		10: R_MIPS_LO16	_mcount
 *		10: R_MIPS_NONE	*ABS*
 *		10: R_MIPS_NONE	*ABS*
 * 14:	03e0082d	move	at,ra
 * 18:	0060f809	jalr	v1
 * label:
 */
#define MIPS_FAKEMCOUNT_OFFSET	4

static int MIPS_is_fake_mcount(Elf_Rel const *rp)
{
	static Elf_Addr old_r_offset = ~(Elf_Addr)0;
	Elf_Addr current_r_offset = _w(rp->r_offset);
	int is_fake;

	is_fake = (old_r_offset != ~(Elf_Addr)0) &&
		(current_r_offset - old_r_offset == MIPS_FAKEMCOUNT_OFFSET);
	old_r_offset = current_r_offset;

	return is_fake;
}

static void append_section(uint_t const *const mloc0,
			   uint_t const *const mlocp,
			   Elf_Rel const *const mrel0,
			   Elf_Rel const *const mrelp,
			   char const *name,
			   unsigned int const rel_entsize,
			   unsigned int const symsec_sh_link,
			   uint_t *name_offp,
			   uint_t *tp,
			   unsigned *shnump
		)
{
	Elf_Shdr mcsec;
	uint_t name_off = *name_offp;
	uint_t t = *tp;
	uint_t loc_diff = (void *)mlocp - (void *)mloc0;
	uint_t rel_diff = (void *)mrelp - (void *)mrel0;
	unsigned shnum = *shnump;

	mcsec.sh_name = w((sizeof(Elf_Rela) == rel_entsize) + strlen(".rel")
		+ name_off);
	mcsec.sh_type = w(SHT_PROGBITS);
	mcsec.sh_flags = _w(SHF_ALLOC);
	mcsec.sh_addr = 0;
	mcsec.sh_offset = _w(t);
	mcsec.sh_size = _w(loc_diff);
	mcsec.sh_link = 0;
	mcsec.sh_info = 0;
	mcsec.sh_addralign = _w(_size);
	mcsec.sh_entsize = _w(_size);
	uwrite(fd_map, &mcsec, sizeof(mcsec));
	t += loc_diff;

	mcsec.sh_name = w(name_off);
	mcsec.sh_type = (sizeof(Elf_Rela) == rel_entsize)
		? w(SHT_RELA)
		: w(SHT_REL);
	mcsec.sh_flags = 0;
	mcsec.sh_addr = 0;
	mcsec.sh_offset = _w(t);
	mcsec.sh_size   = _w(rel_diff);
	mcsec.sh_link = w(symsec_sh_link);
	mcsec.sh_info = w(shnum);
	mcsec.sh_addralign = _w(_size);
	mcsec.sh_entsize = _w(rel_entsize);
	uwrite(fd_map, &mcsec, sizeof(mcsec));
	t += rel_diff;

	shnum += 2;
	name_off += strlen(name) + 1;

	*tp = t;
	*shnump = shnum;
	*name_offp = name_off;
}

/* Append the new shstrtab, Elf_Shdr[], __mcount_loc and its relocations. */
static void append_func(Elf_Ehdr *const ehdr,
			Elf_Shdr *const shstr,
			uint_t const *const mloc0,
			uint_t const *const mlocp,
			Elf_Rel const *const mrel0,
			Elf_Rel const *const mrelp,
			uint_t const *const mloc0_u,
			uint_t const *const mlocp_u,
			Elf_Rel const *const mrel0_u,
			Elf_Rel const *const mrelp_u,
			uint_t const *const mloc0_i,
			uint_t const *const mlocp_i,
			Elf_Rel const *const mrel0_i,
			Elf_Rel const *const mrelp_i,
			unsigned int const rel_entsize,
			unsigned int const symsec_sh_link)
{
	/* Begin constructing output file */
	char const *mc_name = (sizeof(Elf_Rela) == rel_entsize)
		? ".rela__mcount_loc"
		:  ".rel__mcount_loc";
	char const *udiv_name = (sizeof(Elf_Rela) == rel_entsize)
		? ".rela__udiv_loc"
		:  ".rel__udiv_loc";
	char const *idiv_name = (sizeof(Elf_Rela) == rel_entsize)
		? ".rela__idiv_loc"
		:  ".rel__idiv_loc";
	unsigned old_shnum = w2(ehdr->e_shnum);
	uint_t const old_shoff = _w(ehdr->e_shoff);
	uint_t const old_shstr_sh_size   = _w(shstr->sh_size);
	uint_t const old_shstr_sh_offset = _w(shstr->sh_offset);
	uint_t new_e_shoff;
	uint_t t = _w(shstr->sh_size);
	uint_t name_off = old_shstr_sh_size;
	int mc = 0, udiv = 0, idiv = 0;
	int num_sections;

	if (mlocp != mloc0) {
		t += 1 + strlen(mc_name);
		mc = 1;
	}
	if (mlocp_u != mloc0_u) {
		t += 1 + strlen(udiv_name);
		udiv = 1;
	}
	if (mlocp_i != mloc0_i) {
		t += 1 + strlen(idiv_name);
		idiv = 1;
	}
	num_sections = (mc * 2) + (udiv * 2) + (idiv * 2);

	shstr->sh_size = _w(t);
	shstr->sh_offset = _w(sb.st_size);
	t += sb.st_size;
	t += (_align & -t);  /* word-byte align */
	new_e_shoff = t;

	/* body for new shstrtab */
	ulseek(fd_map, sb.st_size, SEEK_SET);
	uwrite(fd_map, old_shstr_sh_offset + (void *)ehdr, name_off);
	if (mc)
		uwrite(fd_map, mc_name, 1 + strlen(mc_name));
	if (udiv)
		uwrite(fd_map, udiv_name, 1 + strlen(udiv_name));
	if (idiv)
		uwrite(fd_map, idiv_name, 1 + strlen(idiv_name));

	/* old(modified) Elf_Shdr table, word-byte aligned */
	ulseek(fd_map, t, SEEK_SET);
	t += sizeof(Elf_Shdr) * old_shnum;
	uwrite(fd_map, old_shoff + (void *)ehdr,
	       sizeof(Elf_Shdr) * old_shnum);

	/* new sections __mcount_loc and .rel__mcount_loc */
	t += num_sections * sizeof(Elf_Shdr);
	if (mc)
		append_section(mloc0, mlocp, mrel0, mrelp, mc_name, rel_entsize,
			       symsec_sh_link, &name_off, &t, &old_shnum);

	/* new sections __udiv_loc and .rel__udiv_loc */
	if (udiv)
		append_section(mloc0_u, mlocp_u, mrel0_u, mrelp_u, udiv_name,
			       rel_entsize, symsec_sh_link, &name_off, &t,
			       &old_shnum);

	/* new sections __idiv_loc and .rel__idiv_loc */
	if (idiv)
		append_section(mloc0_i, mlocp_i, mrel0_i, mrelp_i, idiv_name,
			       rel_entsize, symsec_sh_link, &name_off, &t,
			       &old_shnum);

	if (mc) {
		uwrite(fd_map, mloc0, (void *)mlocp - (void *)mloc0);
		uwrite(fd_map, mrel0, (void *)mrelp - (void *)mrel0);
	}
	if (udiv) {
		uwrite(fd_map, mloc0_u, (void *)mlocp_u - (void *)mloc0_u);
		uwrite(fd_map, mrel0_u, (void *)mrelp_u - (void *)mrel0_u);
	}
	if (idiv) {
		uwrite(fd_map, mloc0_i, (void *)mlocp_i - (void *)mloc0_i);
		uwrite(fd_map, mrel0_i, (void *)mrelp_i - (void *)mrel0_i);
	}

	ehdr->e_shoff = _w(new_e_shoff);
	ehdr->e_shnum = w2(num_sections + w2(ehdr->e_shnum));
	ulseek(fd_map, 0, SEEK_SET);
	uwrite(fd_map, ehdr, sizeof(*ehdr));
}

static unsigned get_mcountsym(Elf_Sym const *const sym0,
			      Elf_Rel const *relp,
			      char const *const str0)
{
	unsigned mcountsym = 0;

	Elf_Sym const *const symp =
		&sym0[Elf_r_sym(relp)];
	char const *symname = &str0[w(symp->st_name)];
	char const *mcount = gpfx == '_' ? "_mcount" : "mcount";
	char const *fentry = "__fentry__";

	if (symname[0] == '.')
		++symname;  /* ppc64 hack */
	if (strcmp(mcount, symname) == 0 ||
	    (altmcount && strcmp(altmcount, symname) == 0) ||
	    (strcmp(fentry, symname) == 0))
		mcountsym = Elf_r_sym(relp);

	return mcountsym;
}

static unsigned get_arm_sym(Elf_Sym const *const sym0,
			      Elf_Rel const *relp,
			      char const *const str0, const char *find)
{
	unsigned sym = 0;
	Elf_Sym const *const symp = &sym0[Elf_r_sym(relp)];
	char const *symname = &str0[w(symp->st_name)];

	if (strcmp(find, symname) == 0)
		sym = Elf_r_sym(relp);

	return sym;
}

static void get_sym_str_and_relp(Elf_Shdr const *const relhdr,
				 Elf_Ehdr const *const ehdr,
				 Elf_Sym const **sym0,
				 char const **str0,
				 Elf_Rel const **relp)
{
	Elf_Shdr *const shdr0 = (Elf_Shdr *)(_w(ehdr->e_shoff)
		+ (void *)ehdr);
	unsigned const symsec_sh_link = w(relhdr->sh_link);
	Elf_Shdr const *const symsec = &shdr0[symsec_sh_link];
	Elf_Shdr const *const strsec = &shdr0[w(symsec->sh_link)];
	Elf_Rel const *const rel0 = (Elf_Rel const *)(_w(relhdr->sh_offset)
		+ (void *)ehdr);

	*sym0 = (Elf_Sym const *)(_w(symsec->sh_offset)
				  + (void *)ehdr);

	*str0 = (char const *)(_w(strsec->sh_offset)
			       + (void *)ehdr);

	*relp = rel0;
}

static void add_relocation(Elf_Rel const *relp, uint_t *mloc0, uint_t **mlocpp,
			   uint_t const recval, unsigned const recsym,
			   Elf_Rel **const mrelpp, unsigned offbase,
			   unsigned rel_entsize, unsigned const reltype)
{
	uint_t *mlocp = *mlocpp;
	Elf_Rel *mrelp = *mrelpp;
	uint_t const addend = _w(_w(relp->r_offset) - recval + mcount_adjust);
	mrelp->r_offset = _w(offbase + ((void *)mlocp - (void *)mloc0));
	Elf_r_info(mrelp, recsym, reltype);
	if (rel_entsize == sizeof(Elf_Rela)) {
		((Elf_Rela *)mrelp)->r_addend = addend;
		*mlocp++ = 0;
	} else
		*mlocp++ = addend;

	*mlocpp = mlocp;
	*mrelpp = (Elf_Rel *)(rel_entsize + (void *)mrelp);
}

/*
 * Look at the relocations in order to find the calls to mcount.
 * Accumulate the section offsets that are found, and their relocation info,
 * onto the end of the existing arrays.
 */
static void sift_rel_mcount(uint_t **mlocpp,
			       uint_t *mloc_base,
			       Elf_Rel **const mrelpp,
			       uint_t **mlocpp_u,
			       uint_t *mloc_base_u,
			       Elf_Rel **const mrelpp_u,
			       uint_t **mlocpp_i,
			       uint_t *mloc_base_i,
			       Elf_Rel **const mrelpp_i,
			       Elf_Shdr const *const relhdr,
			       Elf_Ehdr const *const ehdr,
			       unsigned const recsym,
			       uint_t const recval,
			       unsigned const reltype)
{
	uint_t *mlocp = *mlocpp;
	unsigned const offbase = (void *)mlocp - (void *)mloc_base;
	uint_t *const mloc0 = mlocp;
	Elf_Rel *mrelp = *mrelpp;
	uint_t *mlocp_u = *mlocpp_u;
	unsigned const offbase_u = (void *)mlocp_u - (void *)mloc_base_u;
	uint_t *const mloc0_u = mlocp_u;
	Elf_Rel *mrelp_u = *mrelpp_u;
	uint_t *mlocp_i = *mlocpp_i;
	unsigned const offbase_i = (void *)mlocp_i - (void *)mloc_base_i;
	uint_t *const mloc0_i = mlocp_i;
	Elf_Rel *mrelp_i = *mrelpp_i;
	Elf_Sym const *sym0;
	char const *str0;
	Elf_Rel const *relp;
	unsigned rel_entsize = _w(relhdr->sh_entsize);
	unsigned const nrel = _w(relhdr->sh_size) / rel_entsize;
	int arm = w2(ehdr->e_machine) == EM_ARM;
	unsigned mcountsym = 0, udiv_sym = 0, idiv_sym =0;
	unsigned t;

	get_sym_str_and_relp(relhdr, ehdr, &sym0, &str0, &relp);

	for (t = nrel; t; --t) {
		if (trace_mcount && !mcountsym)
			mcountsym = get_mcountsym(sym0, relp, str0);

		if (arm && !udiv_sym)
			udiv_sym = get_arm_sym(sym0, relp, str0,
					       "__aeabi_uidiv");
		if (arm && !idiv_sym)
			idiv_sym = get_arm_sym(sym0, relp, str0,
						 "__aeabi_idiv");

		if (mcountsym == Elf_r_sym(relp) && !is_fake_mcount(relp))
			add_relocation(relp, mloc0, &mlocp, recval, recsym,
				       &mrelp, offbase, rel_entsize, reltype);

		if (udiv_sym == Elf_r_sym(relp)) {
			switch (relp->r_info & 0xff) {
			case R_ARM_PC24:
			case 28:
			case 29:
				add_relocation(relp, mloc0_u, &mlocp_u, recval,
						recsym, &mrelp_u, offbase_u,
						rel_entsize, reltype);
				break;
			default:
				break;
			}
		}

		if (idiv_sym == Elf_r_sym(relp)) {
			switch (relp->r_info & 0xff) {
			case R_ARM_PC24:
			case 28:
			case 29:
				add_relocation(relp, mloc0_i, &mlocp_i, recval,
						recsym, &mrelp_i, offbase_i,
						rel_entsize, reltype);
				break;
			default:
				break;
			}
		}

		relp = (Elf_Rel const *)(rel_entsize + (void *)relp);
	}
	*mrelpp_i = mrelp_i;
	*mrelpp_u = mrelp_u;
	*mrelpp = mrelp;
	*mlocpp_i = mlocp_i;
	*mlocpp_u = mlocp_u;
	*mlocpp = mlocp;
}

/*
 * Read the relocation table again, but this time its called on sections
 * that are not going to be traced. The mcount calls here will be converted
 * into nops.
 */
static void nop_mcount(Elf_Shdr const *const relhdr,
		       Elf_Ehdr const *const ehdr,
		       const char *const txtname)
{
	Elf_Shdr *const shdr0 = (Elf_Shdr *)(_w(ehdr->e_shoff)
		+ (void *)ehdr);
	Elf_Sym const *sym0;
	char const *str0;
	Elf_Rel const *relp;
	Elf_Shdr const *const shdr = &shdr0[w(relhdr->sh_info)];
	unsigned rel_entsize = _w(relhdr->sh_entsize);
	unsigned const nrel = _w(relhdr->sh_size) / rel_entsize;
	unsigned mcountsym = 0;
	unsigned t;
	int once = 0;

	get_sym_str_and_relp(relhdr, ehdr, &sym0, &str0, &relp);

	for (t = nrel; t; --t) {
		int ret = -1;

		if (!mcountsym)
			mcountsym = get_mcountsym(sym0, relp, str0);

		if (mcountsym == Elf_r_sym(relp) && !is_fake_mcount(relp)) {
			if (make_nop)
				ret = make_nop((void *)ehdr, shdr->sh_offset + relp->r_offset);
			if (warn_on_notrace_sect && !once) {
				printf("Section %s has mcount callers being ignored\n",
				       txtname);
				once = 1;
				/* just warn? */
				if (!make_nop)
					return;
			}
		}

		/*
		 * If we successfully removed the mcount, mark the relocation
		 * as a nop (don't do anything with it).
		 */
		if (!ret) {
			Elf_Rel rel;
			rel = *(Elf_Rel *)relp;
			Elf_r_info(&rel, Elf_r_sym(relp), rel_type_nop);
			ulseek(fd_map, (void *)relp - (void *)ehdr, SEEK_SET);
			uwrite(fd_map, &rel, sizeof(rel));
		}
		relp = (Elf_Rel const *)(rel_entsize + (void *)relp);
	}
}


/*
 * Find a symbol in the given section, to be used as the base for relocating
 * the table of offsets of calls to mcount.  A local or global symbol suffices,
 * but avoid a Weak symbol because it may be overridden; the change in value
 * would invalidate the relocations of the offsets of the calls to mcount.
 * Often the found symbol will be the unnamed local symbol generated by
 * GNU 'as' for the start of each section.  For example:
 *    Num:    Value  Size Type    Bind   Vis      Ndx Name
 *      2: 00000000     0 SECTION LOCAL  DEFAULT    1
 */
static unsigned find_secsym_ndx(unsigned const txtndx,
				char const *const txtname,
				uint_t *const recvalp,
				Elf_Shdr const *const symhdr,
				Elf_Ehdr const *const ehdr)
{
	Elf_Sym const *const sym0 = (Elf_Sym const *)(_w(symhdr->sh_offset)
		+ (void *)ehdr);
	unsigned const nsym = _w(symhdr->sh_size) / _w(symhdr->sh_entsize);
	Elf_Sym const *symp;
	unsigned t;

	for (symp = sym0, t = nsym; t; --t, ++symp) {
		unsigned int const st_bind = ELF_ST_BIND(symp->st_info);

		if (txtndx == w2(symp->st_shndx)
			/* avoid STB_WEAK */
		    && (STB_LOCAL == st_bind || STB_GLOBAL == st_bind)) {
			/* function symbols on ARM have quirks, avoid them */
			if (w2(ehdr->e_machine) == EM_ARM
			    && ELF_ST_TYPE(symp->st_info) == STT_FUNC)
				continue;

			*recvalp = _w(symp->st_value);
			return symp - sym0;
		}
	}
	fprintf(stderr, "Cannot find symbol for section %d: %s.\n",
		txtndx, txtname);
	fail_file();
}


/* Evade ISO C restriction: no declaration after statement in has_rel_mcount. */
static char const *
__has_rel_mcount(Elf_Shdr const *const relhdr,  /* is SHT_REL or SHT_RELA */
		 Elf_Shdr const *const shdr0,
		 char const *const shstrtab,
		 char const *const fname, const char *find)
{
	/* .sh_info depends on .sh_type == SHT_REL[,A] */
	Elf_Shdr const *const txthdr = &shdr0[w(relhdr->sh_info)];
	char const *const txtname = &shstrtab[w(txthdr->sh_name)];

	if (strcmp(find, txtname) == 0) {
		fprintf(stderr, "warning: %s already exists: %s\n", find,
			fname);
		succeed_file();
	}
	if (w(txthdr->sh_type) != SHT_PROGBITS ||
	    !(_w(txthdr->sh_flags) & SHF_EXECINSTR))
		return NULL;
	return txtname;
}

static char const *has_rel_mcount(Elf_Shdr const *const relhdr,
				  Elf_Shdr const *const shdr0,
				  char const *const shstrtab,
				  char const *const fname, const char *find)
{
	if (w(relhdr->sh_type) != SHT_REL && w(relhdr->sh_type) != SHT_RELA)
		return NULL;
	return __has_rel_mcount(relhdr, shdr0, shstrtab, fname, find);
}


static unsigned tot_relsize(Elf_Shdr const *const shdr0,
			    unsigned nhdr,
			    const char *const shstrtab,
			    const char *const fname, const char *find)
{
	unsigned totrelsz = 0;
	Elf_Shdr const *shdrp = shdr0;
	char const *txtname;

	for (; nhdr; --nhdr, ++shdrp) {
		txtname = has_rel_mcount(shdrp, shdr0, shstrtab, fname, find);
		if (txtname && is_mcounted_section_name(txtname))
			totrelsz += _w(shdrp->sh_size);
	}
	return totrelsz;
}


/* Overall supervision for Elf32 ET_REL file. */
static void
do_func(Elf_Ehdr *const ehdr, char const *const fname, unsigned const reltype)
{
	Elf_Shdr *const shdr0 = (Elf_Shdr *)(_w(ehdr->e_shoff)
		+ (void *)ehdr);
	unsigned const nhdr = w2(ehdr->e_shnum);
	Elf_Shdr *const shstr = &shdr0[w2(ehdr->e_shstrndx)];
	char const *const shstrtab = (char const *)(_w(shstr->sh_offset)
		+ (void *)ehdr);

	Elf_Shdr const *relhdr;
	unsigned k;

	/* Upper bound on space: assume all relevant relocs are for mcount. */
	unsigned const totrelsz = tot_relsize(shdr0, nhdr, shstrtab, fname,
					      "__mcount_loc");
	Elf_Rel *const mrel0 = umalloc(totrelsz);
	Elf_Rel *      mrelp = mrel0;

	/* 2*sizeof(address) <= sizeof(Elf_Rel) */
	uint_t *const mloc0 = umalloc(totrelsz>>1);
	uint_t *      mlocp = mloc0;

	/* Allocate for arm too */
	Elf_Rel *const mrel0_u = umalloc(totrelsz);
	Elf_Rel *      mrelp_u = mrel0_u;

	/* 2*sizeof(address) <= sizeof(Elf_Rel) */
	uint_t *const mloc0_u = umalloc(totrelsz>>1);
	uint_t *      mlocp_u = mloc0_u;

	/* Allocate for arm too */
	Elf_Rel *const mrel0_i = umalloc(totrelsz);
	Elf_Rel *      mrelp_i = mrel0_i;

	/* 2*sizeof(address) <= sizeof(Elf_Rel) */
	uint_t *const mloc0_i = umalloc(totrelsz>>1);
	uint_t *      mlocp_i = mloc0_i;

	unsigned rel_entsize = 0;
	unsigned symsec_sh_link = 0;

	for (relhdr = shdr0, k = nhdr; k; --k, ++relhdr) {
		char const *const txtname = has_rel_mcount(relhdr, shdr0,
			shstrtab, fname, "__mcount_loc");
		if (txtname && is_mcounted_section_name(txtname)) {
			uint_t recval = 0;
			unsigned const recsym = find_secsym_ndx(
				w(relhdr->sh_info), txtname, &recval,
				&shdr0[symsec_sh_link = w(relhdr->sh_link)],
				ehdr);

			rel_entsize = _w(relhdr->sh_entsize);
			sift_rel_mcount(&mlocp, mloc0, &mrelp,
				&mlocp_u, mloc0_u, &mrelp_u, &mlocp_i, mloc0_i,
				&mrelp_i, relhdr, ehdr, recsym, recval,
				reltype);
		} else if (txtname && (warn_on_notrace_sect || make_nop)) {
			/*
			 * This section is ignored by ftrace, but still
			 * has mcount calls. Convert them to nops now.
			 */
			nop_mcount(relhdr, ehdr, txtname);
		}
	}
	if (mloc0 != mlocp || mloc0_u != mlocp_u || mloc0_i != mlocp_i) {
		append_func(ehdr, shstr, mloc0, mlocp, mrel0, mrelp,
			    mloc0_u, mlocp_u, mrel0_u, mrelp_u,
			    mloc0_i, mlocp_i, mrel0_i, mrelp_i,
			    rel_entsize, symsec_sh_link);
	}
	free(mrel0);
	free(mloc0);
	free(mrel0_u);
	free(mloc0_u);
	free(mrel0_i);
	free(mloc0_i);
}
