/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Look at an ELF file's .note.gnu.property and determine if the file
 * supports shadow stack and/or indirect branch tracking.
 * The path from the ELF header to the note section is the following:
 * elfhdr->elf_phdr->elf_note->property[].
 */

#include <asm/cet.h>
#include <asm/elf_property.h>
#include <uapi/linux/elf-em.h>
#include <linux/binfmts.h>
#include <linux/elf.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/compat.h>

/*
 * The .note.gnu.property layout:
 *
 *	struct elf_note {
 *		u32 n_namesz; --> sizeof(n_name[]); always (4)
 *		u32 n_ndescsz;--> sizeof(property[])
 *		u32 n_type;   --> always NT_GNU_PROPERTY_TYPE_0
 *	};
 *
 *	char n_name[4]; --> always 'GNU\0'
 *
 *	struct {
 *		u32 pr_type;
 *		u32 pr_datasz;--> sizeof(pr_data[])
 *		u8  pr_data[pr_datasz];
 *	} property[];
 */

#define ELF_NOTE_DESC_OFFSET(n, align) \
	round_up(sizeof(*n) + n->n_namesz, (align))

#define ELF_NOTE_NEXT_OFFSET(n, align) \
	round_up(ELF_NOTE_DESC_OFFSET(n, align) + n->n_descsz, (align))

#define NOTE_PROPERTY_TYPE_0(n) \
	((n->n_namesz == 4) && (memcmp(n + 1, "GNU", 4) == 0) && \
	 (n->n_type == NT_GNU_PROPERTY_TYPE_0))

#define NOTE_SIZE_BAD(n, align, max) \
	((n->n_descsz < 8) || ((n->n_descsz % align) != 0) || \
	 (((u8 *)(n + 1) + 4 + n->n_descsz) > (max)))

/*
 * Go through the property array and look for the one
 * with pr_type of GNU_PROPERTY_X86_FEATURE_1_AND.
 */
static u32 find_x86_feature_1(u8 *buf, u32 size, u32 align)
{
	u8 *end = buf + size;
	u8 *ptr = buf;

	while (1) {
		u32 pr_type, pr_datasz;

		if ((ptr + 4) >= end)
			break;

		pr_type = *(u32 *)ptr;
		pr_datasz = *(u32 *)(ptr + 4);
		ptr += 8;

		if ((ptr + pr_datasz) >= end)
			break;

		if (pr_type == GNU_PROPERTY_X86_FEATURE_1_AND &&
		    pr_datasz == 4)
			return *(u32 *)ptr;

		ptr += pr_datasz;
	}
	return 0;
}

static int find_cet(u8 *buf, u32 size, u32 align, int *shstk, int *ibt)
{
	struct elf_note *note = (struct elf_note *)buf;
	*shstk = 0;
	*ibt = 0;

	/*
	 * Go through the note section and find the note
	 * with n_type of NT_GNU_PROPERTY_TYPE_0.
	 */
	while ((unsigned long)(note + 1) - (unsigned long)buf < size) {
		if (NOTE_PROPERTY_TYPE_0(note)) {
			u32 p;

			if (NOTE_SIZE_BAD(note, align, buf + size))
				return 0;

			/*
			 * Found the note; look at its property array.
			 */
			p = find_x86_feature_1((u8 *)(note + 1) + 4,
					       note->n_descsz, align);

			if (p & GNU_PROPERTY_X86_FEATURE_1_SHSTK)
				*shstk = 1;
			if (p & GNU_PROPERTY_X86_FEATURE_1_IBT)
				*ibt = 1;
			return 1;
		}

		/*
		 * Note sections like .note.ABI-tag and .note.gnu.build-id
		 * are aligned to 4 bytes in 64-bit ELF objects.  So always
		 * use phdr->p_align.
		 */
		note = (void *)note + ELF_NOTE_NEXT_OFFSET(note, align);
	}

	return 0;
}

static int check_pt_note_segment(struct file *file,
				 unsigned long note_size, loff_t *pos,
				 u32 align, int *shstk, int *ibt)
{
	int retval;
	char *note_buf;

	/*
	 * PT_NOTE segment is small.  Read at most
	 * PAGE_SIZE.
	 */
	if (note_size > PAGE_SIZE)
		note_size = PAGE_SIZE;

	/*
	 * Try to read in the whole PT_NOTE segment.
	 */
	note_buf = kmalloc(note_size, GFP_KERNEL);
	if (!note_buf)
		return -ENOMEM;
	retval = kernel_read(file, note_buf, note_size, pos);
	if (retval != note_size) {
		kfree(note_buf);
		return (retval < 0) ? retval : -EIO;
	}

	retval = find_cet(note_buf, note_size, align, shstk, ibt);
	kfree(note_buf);
	return retval;
}

#ifdef CONFIG_COMPAT
static int check_pt_note_32(struct file *file, struct elf32_phdr *phdr,
			    int phnum, int *shstk, int *ibt)
{
	int i;
	int found = 0;

	/*
	 * Go through all PT_NOTE segments and find NT_GNU_PROPERTY_TYPE_0.
	 */
	for (i = 0; i < phnum; i++, phdr++) {
		loff_t pos;

		/*
		 * NT_GNU_PROPERTY_TYPE_0 note is aligned to 4 bytes
		 * in 32-bit binaries.
		 */
		if ((phdr->p_type != PT_NOTE) || (phdr->p_align != 4))
			continue;

		pos = phdr->p_offset;
		found = check_pt_note_segment(file, phdr->p_filesz,
					      &pos, phdr->p_align,
					      shstk, ibt);
		if (found)
			break;
	}
	return found;
}
#endif

#ifdef CONFIG_X86_64
static int check_pt_note_64(struct file *file, struct elf64_phdr *phdr,
			    int phnum, int *shstk, int *ibt)
{
	int found = 0;

	/*
	 * Go through all PT_NOTE segments.
	 */
	for (; phnum > 0; phnum--, phdr++) {
		loff_t pos;

		/*
		 * NT_GNU_PROPERTY_TYPE_0 note is aligned to 8 bytes
		 * in 64-bit binaries.
		 */
		if ((phdr->p_type != PT_NOTE) || (phdr->p_align != 8))
			continue;

		pos = phdr->p_offset;
		found = check_pt_note_segment(file, phdr->p_filesz,
					      &pos, phdr->p_align,
					      shstk, ibt);

		if (found)
			break;
	}
	return found;
}
#endif

int arch_setup_features(void *ehdr_p, void *phdr_p,
			struct file *file, bool interp)
{
	int err = 0;
	int shstk = 0;
	int ibt = 0;

	struct elf64_hdr *ehdr64 = ehdr_p;

	if (!cpu_feature_enabled(X86_FEATURE_SHSTK) &&
	    !cpu_feature_enabled(X86_FEATURE_IBT))
		return 0;

	if (ehdr64->e_ident[EI_CLASS] == ELFCLASS64) {
		struct elf64_phdr *phdr64 = phdr_p;

		err = check_pt_note_64(file, phdr64, ehdr64->e_phnum,
				       &shstk, &ibt);
		if (err < 0)
			goto out;
	} else {
#ifdef CONFIG_COMPAT
		struct elf32_hdr *ehdr32 = ehdr_p;

		if (ehdr32->e_ident[EI_CLASS] == ELFCLASS32) {
			struct elf32_phdr *phdr32 = phdr_p;

			err = check_pt_note_32(file, phdr32, ehdr32->e_phnum,
					       &shstk, &ibt);
			if (err < 0)
				goto out;
		}
#endif
	}

	current->thread.cet.shstk_enabled = 0;
	current->thread.cet.shstk_base = 0;
	current->thread.cet.shstk_size = 0;
	current->thread.cet.ibt_enabled = 0;
	current->thread.cet.ibt_bitmap_addr = 0;
	current->thread.cet.ibt_bitmap_size = 0;
	if (cpu_feature_enabled(X86_FEATURE_SHSTK)) {
		if (shstk) {
			err = cet_setup_shstk();
			if (err < 0)
				goto out;
		}
	}

	if (cpu_feature_enabled(X86_FEATURE_IBT)) {
		if (ibt) {
			err = cet_setup_ibt();
			if (err < 0)
				goto out;
		}
	}

out:
	return err;
}
