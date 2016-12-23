#ifndef _ASM_KEXEC_BZIMAGE64_H
#define _ASM_KEXEC_BZIMAGE64_H

struct sha_region {
	unsigned long start;
	unsigned long len;
};

extern struct kexec_file_ops kexec_bzImage64_ops;

/* needed for kexec_purgatory_get_set_symbol() */
extern unsigned long backup_dest;
extern unsigned long backup_src;
extern unsigned long backup_sz;
extern u8 sha256_digest[];
extern struct sha_region sha_regions[];

void purgatory(void);
int copy_backup_region(void);
int verify_sha256_digest(void);

#endif  /* _ASM_KEXE_BZIMAGE64_H */
