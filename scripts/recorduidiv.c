/*
 * recorduidiv.c: construct a table of the locations of calls to '__aeabi_uidiv'
 * and '__aeabi_idiv' so that the kernel can replace them with idiv and sdiv
 * instructions.
 *
 * Copyright 2009 John F. Reiser <jreiser@BitWagon.com>.  All rights reserved.
 * Licensed under the GNU General Public License, version 2 (GPLv2).
 *
 * Restructured to fit Linux format, as well as other updates:
 *  Copyright 2010 Steven Rostedt <srostedt@redhat.com>, Red Hat Inc.
 *
 * Copyright (c) 2015 The Linux Foundation. All rights reserved.
 */

/*
 * Strategy: alter the .o file in-place.
 *
 * Append a new STRTAB that has the new section names, followed by a new array
 * ElfXX_Shdr[] that has the new section headers, followed by the section
 * contents for __udiv_loc and __idiv_loc and their relocations. The old
 * shstrtab strings, and the old ElfXX_Shdr[] array, remain as "garbage"
 * (commonly, a couple kilobytes.) Subsequent processing by /bin/ld (or the
 * kernel module loader) will ignore the garbage regions, because they are not
 * designated by the new .e_shoff nor the new ElfXX_Shdr[].  [In order to
 * remove the garbage, then use "ld -r" to create a new file that omits the
 * garbage.]
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <elf.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

# define _align			3u
# define _size			4

#ifndef R_ARM_CALL
#define R_ARM_CALL		28
#endif

#ifndef R_ARM_JUMP24
#define R_ARM_JUMP24		29
#endif

static int fd_map;	/* File descriptor for file being modified. */
static int mmap_failed; /* Boolean flag. */
static void *ehdr_curr; /* current ElfXX_Ehdr *  for resource cleanup */
static struct stat sb;	/* Remember .st_size, etc. */
static jmp_buf jmpenv;	/* setjmp/longjmp per-file error escape */

/* setjmp() return values */
enum {
	SJ_SETJMP = 0,  /* hardwired first return */
	SJ_FAIL,
	SJ_SUCCEED
};

/* Per-file resource cleanup when multiple files. */
static void
cleanup(void)
{
	if (!mmap_failed)
		munmap(ehdr_curr, sb.st_size);
	else
		free(ehdr_curr);
	close(fd_map);
}

static void __attribute__((noreturn))
fail_file(void)
{
	cleanup();
	longjmp(jmpenv, SJ_FAIL);
}

static void __attribute__((noreturn))
succeed_file(void)
{
	cleanup();
	longjmp(jmpenv, SJ_SUCCEED);
}

/* ulseek, uread, ...:  Check return value for errors. */

static off_t
ulseek(int const fd, off_t const offset, int const whence)
{
	off_t const w = lseek(fd, offset, whence);
	if (w == (off_t)-1) {
		perror("lseek");
		fail_file();
	}
	return w;
}

static size_t
uread(int const fd, void *const buf, size_t const count)
{
	size_t const n = read(fd, buf, count);
	if (n != count) {
		perror("read");
		fail_file();
	}
	return n;
}

static size_t
uwrite(int const fd, void const *const buf, size_t const count)
{
	size_t const n = write(fd, buf, count);
	if (n != count) {
		perror("write");
		fail_file();
	}
	return n;
}

static void *
umalloc(size_t size)
{
	void *const addr = malloc(size);
	if (addr == 0) {
		fprintf(stderr, "malloc failed: %zu bytes\n", size);
		fail_file();
	}
	return addr;
}

/*
 * Get the whole file as a programming convenience in order to avoid
 * malloc+lseek+read+free of many pieces.  If successful, then mmap
 * avoids copying unused pieces; else just read the whole file.
 * Open for both read and write; new info will be appended to the file.
 * Use MAP_PRIVATE so that a few changes to the in-memory ElfXX_Ehdr
 * do not propagate to the file until an explicit overwrite at the last.
 * This preserves most aspects of consistency (all except .st_size)
 * for simultaneous readers of the file while we are appending to it.
 * However, multiple writers still are bad.  We choose not to use
 * locking because it is expensive and the use case of kernel build
 * makes multiple writers unlikely.
 */
static void *mmap_file(char const *fname)
{
	void *addr;

	fd_map = open(fname, O_RDWR);
	if (fd_map < 0 || fstat(fd_map, &sb) < 0) {
		perror(fname);
		fail_file();
	}
	if (!S_ISREG(sb.st_mode)) {
		fprintf(stderr, "not a regular file: %s\n", fname);
		fail_file();
	}
	addr = mmap(0, sb.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE,
		    fd_map, 0);
	mmap_failed = 0;
	if (addr == MAP_FAILED) {
		mmap_failed = 1;
		addr = umalloc(sb.st_size);
		uread(fd_map, addr, sb.st_size);
	}
	return addr;
}

/* w8rev, w8nat, ...: Handle endianness. */

static uint64_t w8rev(uint64_t const x)
{
	return   ((0xff & (x >> (0 * 8))) << (7 * 8))
	       | ((0xff & (x >> (1 * 8))) << (6 * 8))
	       | ((0xff & (x >> (2 * 8))) << (5 * 8))
	       | ((0xff & (x >> (3 * 8))) << (4 * 8))
	       | ((0xff & (x >> (4 * 8))) << (3 * 8))
	       | ((0xff & (x >> (5 * 8))) << (2 * 8))
	       | ((0xff & (x >> (6 * 8))) << (1 * 8))
	       | ((0xff & (x >> (7 * 8))) << (0 * 8));
}

static uint32_t w4rev(uint32_t const x)
{
	return   ((0xff & (x >> (0 * 8))) << (3 * 8))
	       | ((0xff & (x >> (1 * 8))) << (2 * 8))
	       | ((0xff & (x >> (2 * 8))) << (1 * 8))
	       | ((0xff & (x >> (3 * 8))) << (0 * 8));
}

static uint32_t w2rev(uint16_t const x)
{
	return   ((0xff & (x >> (0 * 8))) << (1 * 8))
	       | ((0xff & (x >> (1 * 8))) << (0 * 8));
}

static uint64_t w8nat(uint64_t const x)
{
	return x;
}

static uint32_t w4nat(uint32_t const x)
{
	return x;
}

static uint32_t w2nat(uint16_t const x)
{
	return x;
}

static uint64_t (*w8)(uint64_t);
static uint32_t (*w)(uint32_t);
static uint32_t (*w2)(uint16_t);

/* Names of the sections that could contain calls to __aeabi_{u}idiv() */
static int is_valid_section_name(char const *const txtname)
{
	return strcmp(".text",           txtname) == 0 ||
		strcmp(".ref.text",      txtname) == 0 ||
		strcmp(".sched.text",    txtname) == 0 ||
		strcmp(".spinlock.text", txtname) == 0 ||
		strcmp(".irqentry.text", txtname) == 0 ||
		strcmp(".kprobes.text",  txtname) == 0 ||
		strcmp(".text.unlikely", txtname) == 0 ||
		strcmp(".init.text",     txtname) == 0;
}

static uint32_t Elf32_r_sym(Elf32_Rel const *rp)
{
	return ELF32_R_SYM(w(rp->r_info));
}

static void append_section(uint32_t const *const mloc0,
			   uint32_t const *const mlocp,
			   Elf32_Rel const *const mrel0,
			   Elf32_Rel const *const mrelp,
			   char const *name,
			   unsigned int const rel_entsize,
			   unsigned int const symsec_sh_link,
			   uint32_t *name_offp,
			   uint32_t *tp,
			   unsigned *shnump
		)
{
	Elf32_Shdr mcsec;
	uint32_t name_off = *name_offp;
	uint32_t t = *tp;
	uint32_t loc_diff = (void *)mlocp - (void *)mloc0;
	uint32_t rel_diff = (void *)mrelp - (void *)mrel0;
	unsigned shnum = *shnump;

	mcsec.sh_name = w((sizeof(Elf32_Rela) == rel_entsize) + strlen(".rel")
		+ name_off);
	mcsec.sh_type = w(SHT_PROGBITS);
	mcsec.sh_flags = w(SHF_ALLOC);
	mcsec.sh_addr = 0;
	mcsec.sh_offset = w(t);
	mcsec.sh_size = w(loc_diff);
	mcsec.sh_link = 0;
	mcsec.sh_info = 0;
	mcsec.sh_addralign = w(_size);
	mcsec.sh_entsize = w(_size);
	uwrite(fd_map, &mcsec, sizeof(mcsec));
	t += loc_diff;

	mcsec.sh_name = w(name_off);
	mcsec.sh_type = (sizeof(Elf32_Rela) == rel_entsize)
		? w(SHT_RELA)
		: w(SHT_REL);
	mcsec.sh_flags = 0;
	mcsec.sh_addr = 0;
	mcsec.sh_offset = w(t);
	mcsec.sh_size   = w(rel_diff);
	mcsec.sh_link = w(symsec_sh_link);
	mcsec.sh_info = w(shnum);
	mcsec.sh_addralign = w(_size);
	mcsec.sh_entsize = w(rel_entsize);
	uwrite(fd_map, &mcsec, sizeof(mcsec));
	t += rel_diff;

	shnum += 2;
	name_off += strlen(name) + 1;

	*tp = t;
	*shnump = shnum;
	*name_offp = name_off;
}

/*
 * Append the new shstrtab, Elf32_Shdr[], __{udiv,idiv}_loc and their
 * relocations.
 */
static void append_func(Elf32_Ehdr *const ehdr,
			Elf32_Shdr *const shstr,
			uint32_t const *const mloc0_u,
			uint32_t const *const mlocp_u,
			Elf32_Rel const *const mrel0_u,
			Elf32_Rel const *const mrelp_u,
			uint32_t const *const mloc0_i,
			uint32_t const *const mlocp_i,
			Elf32_Rel const *const mrel0_i,
			Elf32_Rel const *const mrelp_i,
			unsigned int const rel_entsize,
			unsigned int const symsec_sh_link)
{
	/* Begin constructing output file */
	char const *udiv_name = (sizeof(Elf32_Rela) == rel_entsize)
		? ".rela__udiv_loc"
		:  ".rel__udiv_loc";
	char const *idiv_name = (sizeof(Elf32_Rela) == rel_entsize)
		? ".rela__idiv_loc"
		:  ".rel__idiv_loc";
	unsigned old_shnum = w2(ehdr->e_shnum);
	uint32_t const old_shoff = w(ehdr->e_shoff);
	uint32_t const old_shstr_sh_size   = w(shstr->sh_size);
	uint32_t const old_shstr_sh_offset = w(shstr->sh_offset);
	uint32_t new_e_shoff;
	uint32_t t = w(shstr->sh_size);
	uint32_t name_off = old_shstr_sh_size;
	int udiv = 0, idiv = 0;
	int num_sections;

	if (mlocp_u != mloc0_u) {
		t += 1 + strlen(udiv_name);
		udiv = 1;
	}
	if (mlocp_i != mloc0_i) {
		t += 1 + strlen(idiv_name);
		idiv = 1;
	}
	num_sections = (udiv * 2) + (idiv * 2);

	shstr->sh_size = w(t);
	shstr->sh_offset = w(sb.st_size);
	t += sb.st_size;
	t += (_align & -t);  /* word-byte align */
	new_e_shoff = t;

	/* body for new shstrtab */
	ulseek(fd_map, sb.st_size, SEEK_SET);
	uwrite(fd_map, old_shstr_sh_offset + (void *)ehdr, name_off);
	if (udiv)
		uwrite(fd_map, udiv_name, 1 + strlen(udiv_name));
	if (idiv)
		uwrite(fd_map, idiv_name, 1 + strlen(idiv_name));

	/* old(modified) Elf32_Shdr table, word-byte aligned */
	ulseek(fd_map, t, SEEK_SET);
	t += sizeof(Elf32_Shdr) * old_shnum;
	uwrite(fd_map, old_shoff + (void *)ehdr,
	       sizeof(Elf32_Shdr) * old_shnum);

	t += num_sections * sizeof(Elf32_Shdr);

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

	if (udiv) {
		uwrite(fd_map, mloc0_u, (void *)mlocp_u - (void *)mloc0_u);
		uwrite(fd_map, mrel0_u, (void *)mrelp_u - (void *)mrel0_u);
	}
	if (idiv) {
		uwrite(fd_map, mloc0_i, (void *)mlocp_i - (void *)mloc0_i);
		uwrite(fd_map, mrel0_i, (void *)mrelp_i - (void *)mrel0_i);
	}

	ehdr->e_shoff = w(new_e_shoff);
	ehdr->e_shnum = w2(num_sections + w2(ehdr->e_shnum));
	ulseek(fd_map, 0, SEEK_SET);
	uwrite(fd_map, ehdr, sizeof(*ehdr));
}

static unsigned get_sym(Elf32_Sym const *const sym0,
			      Elf32_Rel const *relp,
			      char const *const str0, const char *find)
{
	unsigned sym = 0;
	Elf32_Sym const *const symp = &sym0[Elf32_r_sym(relp)];
	char const *symname = &str0[w(symp->st_name)];

	if (strcmp(find, symname) == 0)
		sym = Elf32_r_sym(relp);

	return sym;
}

static void get_sym_str_and_relp(Elf32_Shdr const *const relhdr,
				 Elf32_Ehdr const *const ehdr,
				 Elf32_Sym const **sym0,
				 char const **str0,
				 Elf32_Rel const **relp)
{
	Elf32_Shdr *const shdr0 = (Elf32_Shdr *)(w(ehdr->e_shoff)
		+ (void *)ehdr);
	unsigned const symsec_sh_link = w(relhdr->sh_link);
	Elf32_Shdr const *const symsec = &shdr0[symsec_sh_link];
	Elf32_Shdr const *const strsec = &shdr0[w(symsec->sh_link)];
	Elf32_Rel const *const rel0 = (Elf32_Rel const *)(w(relhdr->sh_offset)
		+ (void *)ehdr);

	*sym0 = (Elf32_Sym const *)(w(symsec->sh_offset)
				  + (void *)ehdr);

	*str0 = (char const *)(w(strsec->sh_offset)
			       + (void *)ehdr);

	*relp = rel0;
}

static void add_relocation(Elf32_Rel const *relp, uint32_t *mloc0, uint32_t **mlocpp,
			   uint32_t const recval, unsigned const recsym,
			   Elf32_Rel **const mrelpp, unsigned offbase,
			   unsigned rel_entsize)
{
	uint32_t *mlocp = *mlocpp;
	Elf32_Rel *mrelp = *mrelpp;
	uint32_t const addend = w(w(relp->r_offset) - recval);
	mrelp->r_offset = w(offbase + ((void *)mlocp - (void *)mloc0));
	mrelp->r_info = w(ELF32_R_INFO(recsym, R_ARM_ABS32));
	if (rel_entsize == sizeof(Elf32_Rela)) {
		((Elf32_Rela *)mrelp)->r_addend = addend;
		*mlocp++ = 0;
	} else
		*mlocp++ = addend;

	*mlocpp = mlocp;
	*mrelpp = (Elf32_Rel *)(rel_entsize + (void *)mrelp);
}

/*
 * Look at the relocations in order to find the calls to __aeabi_{u}idiv.
 * Accumulate the section offsets that are found, and their relocation info,
 * onto the end of the existing arrays.
 */
static void sift_relocations(uint32_t **mlocpp_u, uint32_t *mloc_base_u,
			     Elf32_Rel **const mrelpp_u,
			     uint32_t **mlocpp_i,
			     uint32_t *mloc_base_i,
			     Elf32_Rel **const mrelpp_i,
			     Elf32_Shdr const *const relhdr,
			     Elf32_Ehdr const *const ehdr,
			     unsigned const recsym, uint32_t const recval)
{
	uint32_t *mlocp_u = *mlocpp_u;
	unsigned const offbase_u = (void *)mlocp_u - (void *)mloc_base_u;
	uint32_t *const mloc0_u = mlocp_u;
	Elf32_Rel *mrelp_u = *mrelpp_u;
	uint32_t *mlocp_i = *mlocpp_i;
	unsigned const offbase_i = (void *)mlocp_i - (void *)mloc_base_i;
	uint32_t *const mloc0_i = mlocp_i;
	Elf32_Rel *mrelp_i = *mrelpp_i;
	Elf32_Sym const *sym0;
	char const *str0;
	Elf32_Rel const *relp;
	unsigned rel_entsize = w(relhdr->sh_entsize);
	unsigned const nrel = w(relhdr->sh_size) / rel_entsize;
	unsigned udiv_sym = 0, idiv_sym = 0;
	unsigned t;

	get_sym_str_and_relp(relhdr, ehdr, &sym0, &str0, &relp);

	for (t = nrel; t; --t) {
		if (!udiv_sym)
			udiv_sym = get_sym(sym0, relp, str0,
					       "__aeabi_uidiv");
		if (!idiv_sym)
			idiv_sym = get_sym(sym0, relp, str0,
						 "__aeabi_idiv");

		switch (relp->r_info & 0xff) {
		case R_ARM_PC24:
		case R_ARM_CALL:
		case R_ARM_JUMP24:
			if (udiv_sym == Elf32_r_sym(relp)) {
				add_relocation(relp, mloc0_u, &mlocp_u, recval,
					       recsym, &mrelp_u, offbase_u,
					       rel_entsize);
			} else if (idiv_sym == Elf32_r_sym(relp)) {
				add_relocation(relp, mloc0_i, &mlocp_i, recval,
					       recsym, &mrelp_i, offbase_i,
					       rel_entsize);
			}
		}

		relp = (Elf32_Rel const *)(rel_entsize + (void *)relp);
	}
	*mrelpp_i = mrelp_i;
	*mrelpp_u = mrelp_u;
	*mlocpp_i = mlocp_i;
	*mlocpp_u = mlocp_u;
}

/*
 * Find a symbol in the given section, to be used as the base for relocating
 * the table of offsets of calls.  A local or global symbol suffices,
 * but avoid a Weak symbol because it may be overridden; the change in value
 * would invalidate the relocations of the offsets of the calls.
 * Often the found symbol will be the unnamed local symbol generated by
 * GNU 'as' for the start of each section.  For example:
 *    Num:    Value  Size Type    Bind   Vis      Ndx Name
 *      2: 00000000     0 SECTION LOCAL  DEFAULT    1
 */
static unsigned find_secsym_ndx(unsigned const txtndx,
				char const *const txtname,
				uint32_t *const recvalp,
				Elf32_Shdr const *const symhdr,
				Elf32_Ehdr const *const ehdr)
{
	Elf32_Sym const *const sym0 = (Elf32_Sym const *)(w(symhdr->sh_offset)
		+ (void *)ehdr);
	unsigned const nsym = w(symhdr->sh_size) / w(symhdr->sh_entsize);
	Elf32_Sym const *symp;
	unsigned t;

	for (symp = sym0, t = nsym; t; --t, ++symp) {
		unsigned int const st_bind = ELF32_ST_BIND(symp->st_info);

		if (txtndx == w2(symp->st_shndx)
			/* avoid STB_WEAK */
		    && (STB_LOCAL == st_bind || STB_GLOBAL == st_bind)) {
			/* function symbols on ARM have quirks, avoid them */
			if (ELF32_ST_TYPE(symp->st_info) == STT_FUNC)
				continue;

			*recvalp = w(symp->st_value);
			return symp - sym0;
		}
	}
	fprintf(stderr, "Cannot find symbol for section %d: %s.\n",
		txtndx, txtname);
	fail_file();
}


/* Evade ISO C restriction: no declaration after statement in has_rel. */
static char const *
__has_rel(Elf32_Shdr const *const relhdr,  /* is SHT_REL or SHT_RELA */
	  Elf32_Shdr const *const shdr0,
	  char const *const shstrtab,
	  char const *const fname)
{
	/* .sh_info depends on .sh_type == SHT_REL[,A] */
	Elf32_Shdr const *const txthdr = &shdr0[w(relhdr->sh_info)];
	char const *const txtname = &shstrtab[w(txthdr->sh_name)];

	if (strcmp("__idiv_loc", txtname) == 0 ||
	    strcmp("__udiv_loc", txtname) == 0)
		succeed_file();
	if (w(txthdr->sh_type) != SHT_PROGBITS ||
	    !(w(txthdr->sh_flags) & SHF_EXECINSTR))
		return NULL;
	return txtname;
}

static char const *has_rel(Elf32_Shdr const *const relhdr,
				  Elf32_Shdr const *const shdr0,
				  char const *const shstrtab,
				  char const *const fname)
{
	if (w(relhdr->sh_type) != SHT_REL && w(relhdr->sh_type) != SHT_RELA)
		return NULL;
	return __has_rel(relhdr, shdr0, shstrtab, fname);
}


static unsigned tot_relsize(Elf32_Shdr const *const shdr0,
			    unsigned nhdr,
			    const char *const shstrtab,
			    const char *const fname)
{
	unsigned totrelsz = 0;
	Elf32_Shdr const *shdrp = shdr0;
	char const *txtname;

	for (; nhdr; --nhdr, ++shdrp) {
		txtname = has_rel(shdrp, shdr0, shstrtab, fname);
		if (txtname && is_valid_section_name(txtname))
			totrelsz += w(shdrp->sh_size);
	}
	return totrelsz;
}


/* Overall supervision for Elf32 ET_REL file. */
static void
do_func(Elf32_Ehdr *const ehdr, char const *const fname)
{
	Elf32_Shdr *const shdr0 = (Elf32_Shdr *)(w(ehdr->e_shoff)
		+ (void *)ehdr);
	unsigned const nhdr = w2(ehdr->e_shnum);
	Elf32_Shdr *const shstr = &shdr0[w2(ehdr->e_shstrndx)];
	char const *const shstrtab = (char const *)(w(shstr->sh_offset)
		+ (void *)ehdr);

	Elf32_Shdr const *relhdr;
	unsigned k;

	/* Upper bound on space: assume all relevant relocs are valid. */
	unsigned const totrelsz = tot_relsize(shdr0, nhdr, shstrtab, fname);
	Elf32_Rel *const mrel0_u = umalloc(totrelsz);
	Elf32_Rel *      mrelp_u = mrel0_u;

	/* 2*sizeof(address) <= sizeof(Elf32_Rel) */
	uint32_t *const mloc0_u = umalloc(totrelsz>>1);
	uint32_t *      mlocp_u = mloc0_u;

	Elf32_Rel *const mrel0_i = umalloc(totrelsz);
	Elf32_Rel *      mrelp_i = mrel0_i;

	/* 2*sizeof(address) <= sizeof(Elf32_Rel) */
	uint32_t *const mloc0_i = umalloc(totrelsz>>1);
	uint32_t *      mlocp_i = mloc0_i;

	unsigned rel_entsize = 0;
	unsigned symsec_sh_link = 0;

	for (relhdr = shdr0, k = nhdr; k; --k, ++relhdr) {
		char const *const txtname = has_rel(relhdr, shdr0, shstrtab,
						    fname);
		if (txtname && is_valid_section_name(txtname)) {
			uint32_t recval = 0;
			unsigned const recsym = find_secsym_ndx(
				w(relhdr->sh_info), txtname, &recval,
				&shdr0[symsec_sh_link = w(relhdr->sh_link)],
				ehdr);

			rel_entsize = w(relhdr->sh_entsize);
			sift_relocations(&mlocp_u, mloc0_u, &mrelp_u,
					&mlocp_i, mloc0_i, &mrelp_i,
					relhdr, ehdr, recsym, recval);
		}
	}
	if (mloc0_u != mlocp_u || mloc0_i != mlocp_i) {
		append_func(ehdr, shstr, mloc0_u, mlocp_u, mrel0_u, mrelp_u,
			    mloc0_i, mlocp_i, mrel0_i, mrelp_i,
			    rel_entsize, symsec_sh_link);
	}
	free(mrel0_u);
	free(mloc0_u);
	free(mrel0_i);
	free(mloc0_i);
}

static void
do_file(char const *const fname)
{
	Elf32_Ehdr *const ehdr = mmap_file(fname);

	ehdr_curr = ehdr;
	w = w4nat;
	w2 = w2nat;
	w8 = w8nat;
	switch (ehdr->e_ident[EI_DATA]) {
		static unsigned int const endian = 1;
	default:
		fprintf(stderr, "unrecognized ELF data encoding %d: %s\n",
			ehdr->e_ident[EI_DATA], fname);
		fail_file();
		break;
	case ELFDATA2LSB:
		if (*(unsigned char const *)&endian != 1) {
			/* main() is big endian, file.o is little endian. */
			w = w4rev;
			w2 = w2rev;
			w8 = w8rev;
		}
		break;
	case ELFDATA2MSB:
		if (*(unsigned char const *)&endian != 0) {
			/* main() is little endian, file.o is big endian. */
			w = w4rev;
			w2 = w2rev;
			w8 = w8rev;
		}
		break;
	}  /* end switch */
	if (memcmp(ELFMAG, ehdr->e_ident, SELFMAG) != 0
	||  w2(ehdr->e_type) != ET_REL
	||  ehdr->e_ident[EI_VERSION] != EV_CURRENT) {
		fprintf(stderr, "unrecognized ET_REL file %s\n", fname);
		fail_file();
	}

	if (w2(ehdr->e_ehsize) != sizeof(Elf32_Ehdr)
	||  w2(ehdr->e_shentsize) != sizeof(Elf32_Shdr)) {
		fprintf(stderr,
			"unrecognized ET_REL file: %s\n", fname);
		fail_file();
	}
	do_func(ehdr, fname);

	cleanup();
}

int
main(int argc, char *argv[])
{
	int n_error = 0;  /* gcc-4.3.0 false positive complaint */
	int i;

	if (argc < 2) {
		fprintf(stderr, "usage: recorduidiv file.o...\n");
		return 0;
	}

	/* Process each file in turn, allowing deep failure. */
	for (i = 1; i < argc; i++) {
		char *file = argv[i];
		int const sjval = setjmp(jmpenv);

		switch (sjval) {
		default:
			fprintf(stderr, "internal error: %s\n", file);
			exit(1);
			break;
		case SJ_SETJMP:    /* normal sequence */
			/* Avoid problems if early cleanup() */
			fd_map = -1;
			ehdr_curr = NULL;
			mmap_failed = 1;
			do_file(file);
			break;
		case SJ_FAIL:    /* error in do_file or below */
			++n_error;
			break;
		case SJ_SUCCEED:    /* premature success */
			/* do nothing */
			break;
		}  /* end switch */
	}
	return !!n_error;
}
