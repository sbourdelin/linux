#ifndef _KEYS_PLATFORM_KEYRING_H
#define _KEYS_PLATFORM_KEYRING_H

#include <linux/key.h>

#ifdef CONFIG_INTEGRITY_PLATFORM_KEYRING

extern const struct key* __init integrity_get_platform_keyring(void);

#endif /* CONFIG_INTEGRITY_PLATFORM_KEYRING */

#endif /* _KEYS_SYSTEM_KEYRING_H */
