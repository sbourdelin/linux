#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/of.h>

#include "of_helpers.h"

/**
 * pseries_of_derive_parent - basically like dirname(1)
 * @path:  the full_name of a node to be added to the tree
 *
 * Returns the node which should be the parent of the node
 * described by path.  E.g., for path = "/foo/bar", returns
 * the node with full_name = "/foo".
 */
struct device_node *pseries_of_derive_parent(const char *path)
{
	struct device_node *parent;
	char *parent_path = "/";
	const char *tail;

	/* We do not want the trailing '/' character */
	tail = kbasename(path) - 1;

	/* reject if path is "/" */
	if (!strcmp(path, "/"))
		return ERR_PTR(-EINVAL);

	if (tail > path) {
		parent_path = kstrndup(path, tail - path, GFP_KERNEL);
		if (!parent_path)
			return ERR_PTR(-ENOMEM);
	}
	parent = of_find_node_by_path(parent_path);
	if (strcmp(parent_path, "/"))
		kfree(parent_path);
	return parent ? parent : ERR_PTR(-EINVAL);
}


/* Helper Routines to convert between drc_index to cpu numbers */

int of_one_drc_info(struct property **prop, void **curval,
			char **dtype, char **dname,
			u32 *drc_index_start_p,
			u32 *num_sequential_elems_p,
			u32 *sequential_inc_p,
			u32 *last_drc_index_p)
{
	char *drc_type, *drc_name_prefix;
	u32 drc_index_start, num_sequential_elems, dummy;
	u32 sequential_inc, last_drc_index;
	const char *p;
	const __be32 *p2;

	drc_index_start = num_sequential_elems = 0;
	sequential_inc = last_drc_index = 0;

	/* Get drc-type:encode-string */
	p = drc_type = (*curval);
	p = of_prop_next_string(*prop, p);
	if (!p)
		return -EINVAL;

	/* Get drc-name-prefix:encode-string */
	drc_name_prefix = (char *)p;
	p = of_prop_next_string(*prop, p);
	if (!p)
		return -EINVAL;

	/* Get drc-index-start:encode-int */
	p2 = (const __be32 *)p;
	p2 = of_prop_next_u32(*prop, p2, &drc_index_start);
	if (!p2)
		return -EINVAL;

	/* Get/skip drc-name-suffix-start:encode-int */
	p2 = of_prop_next_u32(*prop, p2, &dummy);
	if (!p)
		return -EINVAL;

	/* Get number-sequential-elements:encode-int */
	p2 = of_prop_next_u32(*prop, p2, &num_sequential_elems);
	if (!p2)
		return -EINVAL;

	/* Get sequential-increment:encode-int */
	p2 = of_prop_next_u32(*prop, p2, &sequential_inc);
	if (!p2)
		return -EINVAL;

	/* Get/skip drc-power-domain:encode-int */
	p2 = of_prop_next_u32(*prop, p2, &dummy);
	if (!p2)
		return -EINVAL;

	/* Should now know end of current entry */
	(*curval) = (void *)p2;
	last_drc_index = drc_index_start +
			((num_sequential_elems-1)*sequential_inc);

	if (dtype)
		*dtype = drc_type;
	if (dname)
		*dname = drc_name_prefix;
	if (drc_index_start_p)
		*drc_index_start_p = drc_index_start;
	if (num_sequential_elems_p)
		*num_sequential_elems_p = num_sequential_elems;
	if (sequential_inc_p)
		*sequential_inc_p = sequential_inc;
	if (last_drc_index_p)
		*last_drc_index_p = last_drc_index;

	return 0;
}
EXPORT_SYMBOL(of_one_drc_info);
