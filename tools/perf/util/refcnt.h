/*
 * Atomic reference counter API with debugging feature
 */
#ifndef __PERF_REFCNT_H
#define __PERF_REFCNT_H

#include <linux/atomic.h>

#ifdef REFCNT_DEBUG

struct refcnt_object {
	struct list_head	list;	/* List of objects */
	void			*obj;	/* Object address which has refcnt */
	const char		*name;	/* Object class name */
	struct list_head	head;	/* List head for buffers */
};

#define BACKTRACE_SIZE 32
struct refcnt_buffer {
	struct list_head	list;	/* List of buffers */
	int			count;	/* Count number at recording point */
	int			nr;	/* Number of recorded buffer entries */
	void			*buf[BACKTRACE_SIZE];	/* Backtrace buffer */
};

void refcnt_object__record(void *obj, const char *name, int count);
void refcnt_object__delete(void *obj);

static inline void __refcnt__init(atomic_t *refcnt, void *obj, const char *name)
{
	atomic_set(refcnt, 1);
	refcnt_object__record(obj, name, 1);
}

static inline void __refcnt__get(atomic_t *refcnt, void *obj)
{
	atomic_inc(refcnt);
	refcnt_object__record(obj, NULL, atomic_read(refcnt));
}

static inline int __refcnt__put(atomic_t *refcnt, void *obj)
{
	refcnt_object__record(obj, NULL, -atomic_read(refcnt));
	return atomic_dec_and_test(refcnt);
}

#define refcnt__init(obj, member)	\
	__refcnt__init(&obj->member, obj, #obj)
#define refcnt__exit(obj, member)	\
	refcnt_object__delete(obj)
#define refcnt__get(obj, member)	\
	__refcnt__get(&obj->member, obj)
#define refcnt__put(obj, member)	\
	__refcnt__put(&obj->member, obj)

#else	/* !REFCNT_DEBUG */

#define refcnt__init(obj, member)	atomic_set(&obj->member, 1)
#define refcnt__exit(obj, member)	do { } while (0)
#define refcnt__get(obj, member)	atomic_inc(&obj->member)
#define refcnt__put(obj, member)	atomic_dec_and_test(&obj->member)

#endif	/* !REFCNT_DEBUG */

#endif	/* __PERF_REFCNT_H */
