#ifndef BOOT_PURGATORY_H
#define BOOT_PURGATORY_H

/* This is really just to make sparse happy.
 * Declaring it all static as sparse suggests is not an option as,
 * the symbol information is needed. see kexec_purgatory_get_set_symbol()
 * Technically these prototype and extern declarations are unnecessary
 */
void purgatory(void);
int verify_sha256_digest(void);

extern unsigned long backup_dest;
extern unsigned long backup_src;
extern unsigned long backup_sz;

struct sha_region {
	unsigned long start;
	unsigned long len;
};

extern u8 sha256_digest[];
extern struct sha_region sha_regions[];

#endif
