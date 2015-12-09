/* Refcount backtrace for debugging leaks */
#include "../perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <execinfo.h>	/* For backtrace */

#include "event.h"
#include "debug.h"
#include "util.h"
#include "refcnt.h"
#include "linux/hash.h"

#define REFCNT_HASHBITS	7
#define REFCNT_HASHSZ	(1 << REFCNT_HASHBITS)

/* A root of backtraces (a hash table of the list of refcnt_object)*/
static struct list_head refcnt_root[REFCNT_HASHSZ];
static const char *refcnt_filter;

static void  __attribute__((constructor)) refcnt__init_root(void)
{
	int h;
	const char *filter;

	for (h = 0; h < REFCNT_HASHSZ; h++)
		INIT_LIST_HEAD(&refcnt_root[h]);

	/* glob matching filter */
	filter = getenv("PERF_REFCNT_DEBUG_FILTER");
	if (filter && *filter)
		refcnt_filter = filter;
}

static void refcnt_object__delete(struct refcnt_object *ref)
{
	struct refcnt_buffer *buf;

	while (!list_empty(&ref->head)) {
		buf = list_entry(ref->head.next, struct refcnt_buffer, list);
		list_del_init(&buf->list);
		free(buf);
	}
	list_del_init(&ref->list);
	free(ref);
}

static struct refcnt_object *refcnt_object__find(void *obj)
{
	struct refcnt_object *ref;
	int h = (int)hash_ptr(obj, REFCNT_HASHBITS);

	list_for_each_entry(ref, &refcnt_root[h], list)
		if (ref->obj == obj)
			return ref;

	return NULL;
}

void refcnt__delete(void *addr)
{
	struct refcnt_object *ref = refcnt_object__find(addr);

	/* If !ref, it is already filtered out */
	if (ref)
		refcnt_object__delete(ref);
}

static void refcnt_object__record(struct refcnt_object *ref, int count)
{
	struct refcnt_buffer *buf = malloc(sizeof(*buf));

	if (!buf) {
		pr_debug("REFCNT: Out of memory for %p (%s)\n",
			 ref->obj, ref->name);
		return;
	}
	INIT_LIST_HEAD(&buf->list);
	buf->count = count;
	buf->nr = backtrace(buf->buf, BACKTRACE_SIZE);
	list_add_tail(&buf->list, &ref->head);
}

static struct refcnt_object *refcnt_object__new(void *obj, const char *name)
{
	struct refcnt_object *ref = malloc(sizeof(*ref));
	int h = (int)hash_ptr(obj, REFCNT_HASHBITS);

	if (!ref) {
		pr_debug("REFCNT: Out of memory for %p (%s)\n",
			 obj, name);
		return NULL;
	}
	INIT_LIST_HEAD(&ref->list);
	INIT_LIST_HEAD(&ref->head);
	ref->name = name;
	ref->obj = obj;
	list_add_tail(&ref->list, &refcnt_root[h]);

	return ref;
}

/* This is called via refcnt__init */
void refcnt__recordnew(void *obj, const char *name, int count)
{
	struct refcnt_object *ref;

	if (refcnt_filter && !strglobmatch(name, refcnt_filter))
		return;

	ref = refcnt_object__new(obj, name);
	if (ref)
		refcnt_object__record(ref, count);
}

/* This is called via refcnt__get/put */
void refcnt__record(void *obj, int count)
{
	struct refcnt_object *ref = refcnt_object__find(obj);

	/* If no entry, it should be filtered out */
	if (ref)
		refcnt_object__record(ref, count);
}

static void pr_refcnt_buffer(struct refcnt_buffer *buf)
{
	char **symbuf;
	int i;

	if (!buf)
		return;

	pr_debug("Refcount %s => %d at\n",
		 buf->count >= 0 ? "+1" : "-1",
		 buf->count >= 0 ? buf->count : -buf->count - 1);

	symbuf = backtrace_symbols(buf->buf, buf->nr);
	/* Skip the first one because it is always btrace__record */
	for (i = 1; i < buf->nr; i++) {
		if (symbuf)
			pr_debug("  %s\n", symbuf[i]);
		else
			pr_debug("  [%p]\n", buf->buf[i]);
	}
	free(symbuf);
}

static void pr_refcnt_object(struct refcnt_object *ref)
{
	struct refcnt_buffer *buf;

	pr_debug("Unreclaimed %s@%p\n",
		 ref->name ? ref->name : "(object)", ref->obj);

	list_for_each_entry(buf, &ref->head, list)
		pr_refcnt_buffer(buf);
}

static void  __attribute__((destructor)) refcnt__dump_unreclaimed(void)
{
	struct refcnt_object *ref, *n;
	int h, i = 0;

	for (h = 0; h < REFCNT_HASHSZ; h++)
		if (!list_empty(&refcnt_root[h]))
			goto found;
	return;
found:
	pr_warning("REFCNT: BUG: Unreclaimed objects found.\n");
	for ( ; h < REFCNT_HASHSZ; h++)
		list_for_each_entry_safe(ref, n, &refcnt_root[h], list) {
			if (verbose) {
				pr_debug("==== [%d] ====\n", i);
				pr_refcnt_object(ref);
			}
			refcnt_object__delete(ref);
			i++;
		}

	pr_warning("REFCNT: Total %d objects are not reclaimed.\n", i);
	if (!verbose)
		pr_warning("   To see all backtraces, rerun with -v option\n");
}

