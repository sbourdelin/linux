/*
 * (C) Copyright 2015 Hewlett Packard Enterprise Development LP.
 *
 * dm-pref-path.c
 *
 * Module Author: Ravikanth Nalla
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License, version 2 as published by the Free Software Foundation;
 * either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * dm-pref-path path selector:
 * Handles preferred path load balance policy operations. The key
 * operations of this policy is to select and return user specified
 * path from the current discovered online/ healthy paths(valid_paths).
 * If the user specified path do not exist in the valid_paths list due
 * to path being currently in failed state or user has mentioned wrong
 * device information, it will fall back to round-robin policy, where
 * all the valid-paths are given equal preference.
 *
 */

#include "dm.h"
#include "dm-path-selector.h"

#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/atomic.h>

#define DM_MSG_PREFIX	"multipath pref-path"
#define PP_MIN_IO       10000
#define PP_VERSION	"1.0.0"
#define BUFF_LEN         16

/* Flag for pref_path enablement */
unsigned pref_path_enabled;

/* pref_path major:minor number */
char pref_path[BUFF_LEN];

struct selector {
	struct list_head	valid_paths;
	struct list_head	failed_paths;
};

struct path_info {
	struct list_head	list;
	struct dm_path		*path;
	unsigned		repeat_count;
};

static struct selector *alloc_selector(void)
{
	struct selector *s = kmalloc(sizeof(*s), GFP_KERNEL);

	if (s) {
		INIT_LIST_HEAD(&s->valid_paths);
		INIT_LIST_HEAD(&s->failed_paths);
	}

	return s;
}

static int pf_create(struct path_selector *ps, unsigned argc, char
**argv) {
	struct selector *s = alloc_selector();

	if (!s)
		return -ENOMEM;

	if ((argc == 1) && strlen(argv[0]) < BUFF_LEN) {
		pref_path_enabled = 1;
		snprintf(pref_path, (BUFF_LEN-1), "%s", argv[0]);
	}

	ps->context = s;
	return 0;
}

static void pf_free_paths(struct list_head *paths)
{
	struct path_info *pi, *next;

	list_for_each_entry_safe(pi, next, paths, list) {
		list_del(&pi->list);
		kfree(pi);
	}
}

static void pf_destroy(struct path_selector *ps)
{
	struct selector *s = ps->context;

	pf_free_paths(&s->valid_paths);
	pf_free_paths(&s->failed_paths);
	kfree(s);
	ps->context = NULL;
}

static int pf_status(struct path_selector *ps, struct dm_path *path,
		     status_type_t type, char *result, unsigned maxlen) {
	unsigned sz = 0;
	struct path_info *pi;

	/* When called with NULL path, return selector status/args. */
	if (!path)
		DMEMIT("0 ");
	else {
		pi = path->pscontext;

		if (type == STATUSTYPE_TABLE)
			DMEMIT("%u ", pi->repeat_count);
	}

	return sz;
}

static int pf_add_path(struct path_selector *ps, struct dm_path *path,
		       int argc, char **argv, char **error) {
	struct selector *s = ps->context;
	struct path_info *pi;

	/*
	 * Arguments: [<pref-path>]
	 */
	if (argc > 1) {
		*error = "pref-path ps: incorrect number of arguments";
		return -EINVAL;
	}

	/* Allocate the path information structure */
	pi = kmalloc(sizeof(*pi), GFP_KERNEL);
	if (!pi) {
		*error = "pref-path ps: Error allocating path information";
		return -ENOMEM;
	}

	pi->path = path;
	pi->repeat_count = PP_MIN_IO;

	path->pscontext = pi;

	list_add_tail(&pi->list, &s->valid_paths);

	return 0;
}

static void pf_fail_path(struct path_selector *ps, struct dm_path
*path) {
	struct selector *s = ps->context;
	struct path_info *pi = path->pscontext;

	list_move(&pi->list, &s->failed_paths); }

static int pf_reinstate_path(struct path_selector *ps, struct dm_path
*path) {
	struct selector *s = ps->context;
	struct path_info *pi = path->pscontext;

	list_move_tail(&pi->list, &s->valid_paths);

	return 0;
}

/*
 * Return user preferred path for an I/O.
 */
static struct dm_path *pf_select_path(struct path_selector *ps,
				      unsigned *repeat_count, size_t nr_bytes) {
	struct selector *s = ps->context;
	struct path_info *pi = NULL, *best = NULL;

	if (list_empty(&s->valid_paths))
		return NULL;

	if (pref_path_enabled) {
		/* search for preferred path in the
		*  valid list and then return.
		*/
		list_for_each_entry(pi, &s->valid_paths, list) {
			if (!strcmp(pi->path->dev->name, pref_path)) {
				best = pi;
				*repeat_count = best->repeat_count;
				break;
			}
		}
	}

	/* If preferred path is not enabled/ not available/
	*  offline chose the next path in the list.
	*/
	if (best == NULL && !list_empty(&s->valid_paths)) {
		pi = list_entry(s->valid_paths.next,
			struct path_info, list);
		list_move_tail(&pi->list, &s->valid_paths);
		best = pi;
		*repeat_count = best->repeat_count;
	}

	return best ? best->path : NULL;
}

static struct path_selector_type pf_ps = {
	.name		= "pref-path",
	.module		= THIS_MODULE,
	.table_args	= 1,
	.info_args	= 0,
	.create		= pf_create,
	.destroy	= pf_destroy,
	.status		= pf_status,
	.add_path	= pf_add_path,
	.fail_path	= pf_fail_path,
	.reinstate_path	= pf_reinstate_path,
	.select_path	= pf_select_path,
};

static int __init dm_pf_init(void)
{
	int r = dm_register_path_selector(&pf_ps);

	if (r < 0) {
		DMERR("register failed %d", r);
		return r;
	}

	DMINFO("version " PP_VERSION " loaded");
	return r;
}

static void __exit dm_pf_exit(void)
{
	dm_unregister_path_selector(&pf_ps);
}

module_init(dm_pf_init);
module_exit(dm_pf_exit);

MODULE_DESCRIPTION(DM_NAME "pref-path multipath path selector");
MODULE_AUTHOR("ravikanth.nalla@hpe.com");
MODULE_LICENSE("GPL");
