/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Look at an ELF file's .note.gnu.property and determine if the file
 * supports shadow stack and/or indirect branch tracking.
 * The path from the ELF header to the note section is the following:
 * elfhdr->elf_phdr->elf_note->property[].
 */

#include <asm/cet.h>
#include <asm/elf_property.h>
#include <asm/prctl.h>
#include <asm/processor.h>
#include <uapi/linux/elf-em.h>
#include <uapi/linux/prctl.h>
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
 *	char n_name[4]; --> always 'GNU\0'
 *
 *	struct {
 *		struct property_x86 {
 *			u32 pr_type;
 *			u32 pr_datasz;
 *		};
 *		u8 pr_data[pr_datasz];
 *	}[];
 */

#define BUF_SIZE (PAGE_SIZE / 4)

struct property_x86 {
	u32 pr_type;
	u32 pr_datasz;
};

typedef bool (test_fn)(void *buf, u32 *arg);
typedef void *(next_fn)(void *buf, u32 *arg);

static inline bool test_note_type_0(void *buf, u32 *arg)
{
	struct elf_note *n = buf;

	return ((n->n_namesz == 4) && (memcmp(n + 1, "GNU", 4) == 0) &&
		(n->n_type == NT_GNU_PROPERTY_TYPE_0));
}

static inline void *next_note(void *buf, u32 *arg)
{
	struct elf_note *n = buf;
	u32 align = *arg;
	int size;

	size = round_up(sizeof(*n) + n->n_namesz, align);
	size = round_up(size + n->n_descsz, align);

	if (buf + size < buf)
		return NULL;
	else
		return (buf + size);
}

static inline bool test_property_x86(void *buf, u32 *arg)
{
	struct property_x86 *pr = buf;
	u32 max_type = *arg;

	if (pr->pr_type > max_type)
		*arg = pr->pr_type;

	return (pr->pr_type == GNU_PROPERTY_X86_FEATURE_1_AND);
}

static inline void *next_property(void *buf, u32 *arg)
{
	struct property_x86 *pr = buf;
	u32 max_type = *arg;

	if ((buf + sizeof(*pr) +  pr->pr_datasz < buf) ||
	    (pr->pr_type > GNU_PROPERTY_X86_FEATURE_1_AND) ||
	    (pr->pr_type > max_type))
		return NULL;
	else
		return (buf + sizeof(*pr) + pr->pr_datasz);
}

/*
 * Scan 'buf' for a pattern; return true if found.
 * *pos is the distance from the beginning of buf to where
 * the searched item or the next item is located.
 */
static int scan(u8 *buf, u32 buf_size, int item_size,
		 test_fn test, next_fn next, u32 *arg, u32 *pos)
{
	int found = 0;
	u8 *p, *max;

	max = buf + buf_size;
	if (max < buf)
		return 0;

	p = buf;

	while ((p + item_size < max) && (p + item_size > buf)) {
		if (test(p, arg)) {
			found = 1;
			break;
		}

		p = next(p, arg);
	}

	*pos = (p + item_size <= buf) ? 0 : (u32)(p - buf);
	return found;
}

/*
 * Search a NT_GNU_PROPERTY_TYPE_0 for GNU_PROPERTY_X86_FEATURE_1_AND.
 */
static int find_feature_x86(struct file *file, unsigned long desc_size,
			    loff_t file_offset, u8 *buf, u32 *feature)
{
	u32 buf_pos;
	unsigned long read_size;
	unsigned long done;
	int found = 0;
	int ret = 0;
	u32 last_pr = 0;

	*feature = 0;
	buf_pos = 0;

	for (done = 0; done < desc_size; done += buf_pos) {
		read_size = desc_size - done;
		if (read_size > BUF_SIZE)
			read_size = BUF_SIZE;

		ret = kernel_read(file, buf, read_size, &file_offset);

		if (ret != read_size)
			return (ret < 0) ? ret : -EIO;

		ret = 0;
		found = scan(buf, read_size, sizeof(struct property_x86),
			     test_property_x86, next_property,
			     &last_pr, &buf_pos);

		if ((!buf_pos) || found)
			break;

		file_offset += buf_pos - read_size;
	}

	if (found) {
		struct property_x86 *pr =
			(struct property_x86 *)(buf + buf_pos);

		if (pr->pr_datasz == 4) {
			u32 *max =  (u32 *)(buf + read_size);
			u32 *data = (u32 *)((u8 *)pr + sizeof(*pr));

			if (data + 1 <= max) {
				*feature = *data;
			} else {
				file_offset += buf_pos - read_size;
				file_offset += sizeof(*pr);
				ret = kernel_read(file, feature, 4,
						  &file_offset);
			}
		}
	}

	return ret;
}

/*
 * Search a PT_NOTE segment for the first NT_GNU_PROPERTY_TYPE_0.
 */
static int find_note_type_0(struct file *file, unsigned long note_size,
			    loff_t file_offset, u32 align, u32 *feature)
{
	u8 *buf;
	u32 buf_pos;
	unsigned long read_size;
	unsigned long done;
	int found = 0;
	int ret = 0;

	buf = kmalloc(BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	*feature = 0;
	buf_pos = 0;

	for (done = 0; done < note_size; done += buf_pos) {
		read_size = note_size - done;
		if (read_size > BUF_SIZE)
			read_size = BUF_SIZE;

		ret = kernel_read(file, buf, read_size, &file_offset);

		if (ret != read_size) {
			ret = (ret < 0) ? ret : -EIO;
			kfree(buf);
			return ret;
		}

		/*
		 * item_size = sizeof(struct elf_note) + elf_note.n_namesz.
		 * n_namesz is 4 for the note type we look for.
		 */
		ret = 0;
		found += scan(buf, read_size, sizeof(struct elf_note) + 4,
			      test_note_type_0, next_note,
			      &align, &buf_pos);

		file_offset += buf_pos - read_size;

		if (found == 1) {
			struct elf_note *n =
				(struct elf_note *)(buf + buf_pos);
			u32 start = round_up(sizeof(*n) + n->n_namesz, align);
			u32 total = round_up(start + n->n_descsz, align);

			ret = find_feature_x86(file, n->n_descsz,
					       file_offset + start,
					       buf, feature);
			file_offset += total;
			buf_pos += total;
		} else if (!buf_pos) {
			*feature = 0;
			break;
		}
	}

	kfree(buf);
	return ret;
}

#ifdef CONFIG_COMPAT
static int check_notes_32(struct file *file, struct elf32_phdr *phdr,
			  int phnum, u32 *feature)
{
	int i;
	int err = 0;

	for (i = 0; i < phnum; i++, phdr++) {
		if ((phdr->p_type != PT_NOTE) || (phdr->p_align != 4))
			continue;

		err = find_note_type_0(file, phdr->p_filesz, phdr->p_offset,
				       phdr->p_align, feature);
		if (err)
			return err;
	}

	return 0;
}
#endif

#ifdef CONFIG_X86_64
static int check_notes_64(struct file *file, struct elf64_phdr *phdr,
			  int phnum, u32 *feature)
{
	int i;
	int err = 0;

	for (i = 0; i < phnum; i++, phdr++) {
		if ((phdr->p_type != PT_NOTE) || (phdr->p_align != 8))
			continue;

		err = find_note_type_0(file, phdr->p_filesz, phdr->p_offset,
				       phdr->p_align, feature);
		if (err)
			return err;
	}

	return 0;
}
#endif

int arch_setup_features(void *ehdr_p, void *phdr_p,
			struct file *file, bool interp)
{
	int err = 0;
	u32 feature = 0;

	struct elf64_hdr *ehdr64 = ehdr_p;

	if (!cpu_feature_enabled(X86_FEATURE_SHSTK))
		return 0;

	if (ehdr64->e_ident[EI_CLASS] == ELFCLASS64) {
		struct elf64_phdr *phdr64 = phdr_p;

		err = check_notes_64(file, phdr64, ehdr64->e_phnum,
				     &feature);
		if (err < 0)
			goto out;
	} else {
#ifdef CONFIG_COMPAT
		struct elf32_hdr *ehdr32 = ehdr_p;

		if (ehdr32->e_ident[EI_CLASS] == ELFCLASS32) {
			struct elf32_phdr *phdr32 = phdr_p;

			err = check_notes_32(file, phdr32, ehdr32->e_phnum,
					     &feature);
			if (err < 0)
				goto out;
		}
#endif
	}

	memset(&current->thread.cet, 0, sizeof(struct cet_status));

	if (cpu_feature_enabled(X86_FEATURE_SHSTK)) {
		if (feature & GNU_PROPERTY_X86_FEATURE_1_SHSTK) {
			err = cet_setup_shstk();
			if (err < 0)
				goto out;
		}
	}

out:
	return err;
}
