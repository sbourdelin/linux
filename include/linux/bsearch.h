#ifndef _LINUX_BSEARCH_H
#define _LINUX_BSEARCH_H

#include <linux/types.h>

void *bsearch(const void *key, const void *base, size_t num, size_t size,
	      int (*cmp)(const void *key, const void *elt));

#define BSEARCH(key, base, num, cmp) ({					\
	unsigned long start__ = 0, end__ = (num);			\
	typeof(base) result__ = NULL;					\
	while (start__ < end__) {					\
		unsigned long mid__ = (start__ + end__) / 2;		\
		int ret__ = (cmp)((key), (base) + mid__);		\
		if (ret__ < 0) {					\
			end__ = mid__;					\
		} else if (ret__ > 0) {					\
			start__ = mid__ + 1;				\
		} else {						\
			result__ = (base) + mid__;			\
			break;						\
		}							\
	}								\
	result__;							\
})

#endif /* _LINUX_BSEARCH_H */
