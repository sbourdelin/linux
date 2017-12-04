/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_KEXEC_INTERNAL_H
#define LINUX_KEXEC_INTERNAL_H

#include <linux/kexec.h>

struct kimage *do_kimage_alloc_init(void);
int sanity_check_segment_list(struct kimage *image);
void kimage_free_page_list(struct list_head *list);
void kimage_free(struct kimage *image);
int kimage_load_segment(struct kimage *image, struct kexec_segment *segment);
void kimage_terminate(struct kimage *image);
int kimage_is_destination_range(struct kimage *image,
				unsigned long start, unsigned long end);

extern struct mutex kexec_mutex;

#ifdef CONFIG_KEXEC_FILE
#include <linux/purgatory.h>

/* Alignment required for elf header segment */
#define ELF_CORE_HEADER_ALIGN   4096

/* Misc data about ram ranges needed to prepare elf headers */
struct crash_elf_data {
	struct kimage *image;
	/*
	 * Total number of ram ranges we have after various adjustments for
	 * crash reserved region, etc.
	 */
	unsigned int max_nr_ranges;

	/* Pointer to elf header */
	void *ehdr;
	/* Pointer to next phdr */
	void *bufp;
	struct crash_mem mem;
};

void kimage_file_post_load_cleanup(struct kimage *image);
extern char kexec_purgatory[];
extern size_t kexec_purgatory_size;
#else /* CONFIG_KEXEC_FILE */
static inline void kimage_file_post_load_cleanup(struct kimage *image) { }
#endif /* CONFIG_KEXEC_FILE */
#endif /* LINUX_KEXEC_INTERNAL_H */
