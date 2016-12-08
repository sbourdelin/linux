#ifndef __LINUX_TIDA_H__
#define __LINUX_TIDA_H__

#include <linux/types.h>
#include <linux/spinlock.h>

struct tida {
	unsigned long	*bits;
	unsigned long	alloc;
	spinlock_t	lock;
	int		hint;
};

#define TIDA_INIT(name)				\
	{ .lock = __SPIN_LOCK_UNLOCKED(name.lock), }

#define DEFINE_TIDA(name) struct tida name = TIDA_INIT(name)

void tida_init(struct tida *tida);
void tida_destroy(struct tida *tida);

int tida_get_above(struct tida *tida, int start, gfp_t gfp);
void tida_put(struct tida *tida, int id);

static inline int
tida_get(struct tida *tida, gfp_t gfp)
{
	return tida_get_above(tida, 0, gfp);
}


#endif /* __LINUX_TIDA_H__ */
