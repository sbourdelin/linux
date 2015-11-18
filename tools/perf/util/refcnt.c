/* Refcount backtrace for debugging leaks */
#include "../perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <execinfo.h>	/* For backtrace */

#include "event.h"
#include "debug.h"
#include "util.h"
#include "refcnt.h"

/* A root of backtrace */
static LIST_HEAD(refcnt_root);	/* List head of refcnt object */

static void __refcnt_object__delete(struct refcnt_object *ref)
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

static struct refcnt_object *refcnt__find_object(void *obj)
{
	struct refcnt_object *ref;

	/* TODO: use hash list */
	list_for_each_entry(ref, &refcnt_root, list)
		if (ref->obj == obj)
			return ref;

	return NULL;
}

void refcnt_object__delete(void *addr)
{
	struct refcnt_object *ref = refcnt__find_object(addr);

	if (!ref) {
		pr_debug("REFCNT: Delete uninitialized refcnt: %p\n", addr);
		return;
	}
	__refcnt_object__delete(ref);
}

void refcnt_object__record(void *obj, const char *name, int count)
{
	struct refcnt_object *ref = refcnt__find_object(obj);
	struct refcnt_buffer *buf;

	/* If no entry, allocate new one */
	if (!ref) {
		ref = malloc(sizeof(*ref));
		if (!ref) {
			pr_debug("REFCNT: Out of memory for %p (%s)\n",
				 obj, name);
			return;
		}
		INIT_LIST_HEAD(&ref->list);
		INIT_LIST_HEAD(&ref->head);
		ref->name = name;
		ref->obj = obj;
		list_add_tail(&ref->list, &refcnt_root);
	}

	buf = malloc(sizeof(*buf));
	if (!buf) {
		pr_debug("REFCNT: Out of memory for %p (%s)\n", obj, ref->name);
		return;
	}

	INIT_LIST_HEAD(&buf->list);
	buf->count = count;
	buf->nr = backtrace(buf->buf, BACKTRACE_SIZE);
	list_add_tail(&buf->list, &ref->head);
}

static void pr_refcnt_buffer(struct refcnt_buffer *buf)
{
	char **symbuf;
	int i;

	if (!buf)
		return;
	symbuf = backtrace_symbols(buf->buf, buf->nr);
	/* Skip the first one because it is always btrace__record */
	for (i = 1; i < buf->nr; i++)
		pr_debug("  %s\n", symbuf[i]);
	free(symbuf);
}

void refcnt__dump_unreclaimed(void) __attribute__((destructor));
void refcnt__dump_unreclaimed(void)
{
	struct refcnt_object *ref, *n;
	struct refcnt_buffer *buf;
	int i = 0;

	if (list_empty(&refcnt_root))
		return;

	pr_warning("REFCNT: BUG: Unreclaimed objects found.\n");
	list_for_each_entry_safe(ref, n, &refcnt_root, list) {
		pr_debug("==== [%d] ====\nUnreclaimed %s: %p\n", i,
			 ref->name ? ref->name : "(object)", ref->obj);
		list_for_each_entry(buf, &ref->head, list) {
			pr_debug("Refcount %s => %d at\n",
				 buf->count > 0 ? "+1" : "-1",
				 buf->count > 0 ? buf->count : -buf->count - 1);
			pr_refcnt_buffer(buf);
		}
		__refcnt_object__delete(ref);
		i++;
	}
	pr_warning("REFCNT: Total %d objects are not reclaimed.\n", i);
	if (!verbose)
		pr_warning("   To see all backtraces, rerun with -v option\n");
}

