#ifndef _LINUX_BITOPS_H
#define _LINUX_BITOPS_H

static inline __u32 rol32(__u32 word, unsigned int shift)
{
	return (word << shift) | (word >> ((-shift) & 31));
}

#endif
