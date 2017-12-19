// SPDX-License-Identifier: GPL-2.0
#include <linux/string.h>
#include <linux/if_ether.h>
#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>

#define MAC_PTON_MINLEN		(3 * ETH_ALEN - 1)

bool mac_pton(const char *s, u8 *mac)
{
	int i;

	/* XX:XX:XX:XX:XX:XX */
	if (strnlen(s, MAC_PTON_MINLEN) < MAC_PTON_MINLEN)
		return false;

	/* Don't dirty result unless string is valid MAC. */
	for (i = 0; i < ETH_ALEN; i++) {
		if (!isxdigit(s[i * 3]) || !isxdigit(s[i * 3 + 1]))
			return false;
		if (i != ETH_ALEN - 1 && s[i * 3 + 2] != ':')
			return false;
	}
	for (i = 0; i < ETH_ALEN; i++) {
		mac[i] = (hex_to_bin(s[i * 3]) << 4) | hex_to_bin(s[i * 3 + 1]);
	}
	return true;
}
EXPORT_SYMBOL(mac_pton);

int mac_pton_from_user(const char __user *s, size_t count, u8 *mac)
{
	char buf[MAC_PTON_MINLEN];

	count = min(count, sizeof(buf));
	if (copy_from_user(buf, s, count))
		return -EFAULT;
	return mac_pton(buf, mac) ? 0 : -EINVAL;
}
EXPORT_SYMBOL(mac_pton_from_user);
