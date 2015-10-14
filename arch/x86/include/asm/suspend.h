#ifdef CONFIG_X86_32
# include <asm/suspend_32.h>
#else
# include <asm/suspend_64.h>
#endif

extern int arch_image_info_save(char *dst, char *src, unsigned int limit_len);
extern bool arch_image_info_check(const char *new, const char *old);
