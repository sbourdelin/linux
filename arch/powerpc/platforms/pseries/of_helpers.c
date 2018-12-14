// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "drc: " fmt

#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <asm/prom.h>

#include "of_helpers.h"
#include "pseries.h"

#define	MAX_DRC_NAME_LEN 64

static int drc_debug;
#define dbg(args...) if (drc_debug) { printk(KERN_DEBUG args); }
#define err(arg...) printk(KERN_ERR args);
#define info(arg...) printk(KERN_INFO args);
#define warn(arg...) printk(KERN_WARNING args);

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

static int of_read_drc_info_cell(struct property **prop,
			const __be32 **curval,
			struct of_drc_info *data)
{
	const char *p;
	const __be32 *p2;

	if (!data)
		return -EINVAL;

	/* Get drc-type:encode-string */
	p = data->drc_type = (char*) (*curval);
	p = of_prop_next_string(*prop, p);
	if (!p)
		return -EINVAL;

	/* Get drc-name-prefix:encode-string */
	data->drc_name_prefix = (char *)p;
	p = of_prop_next_string(*prop, p);
	if (!p)
		return -EINVAL;

	/* Get drc-index-start:encode-int */
	p2 = (const __be32 *)p;
	data->drc_index_start = of_read_number(p2++, 1);

	/* Get drc-name-suffix-start:encode-int */
	data->drc_name_suffix_start = of_read_number(p2++, 1);

	/* Get number-sequential-elements:encode-int */
	data->num_sequential_elems = of_read_number(p2++, 1);

	/* Get sequential-increment:encode-int */
	data->sequential_inc = of_read_number(p2++, 1);

	/* Get drc-power-domain:encode-int */
	data->drc_power_domain = of_read_number(p2++, 1);

	/* Should now know end of current entry */
	(*curval) = (void *)p2;
	data->last_drc_index = data->drc_index_start +
		((data->num_sequential_elems - 1) * data->sequential_inc);

	return 0;
}

static int walk_drc_info(struct device_node *dn,
		bool (*usercb)(struct of_drc_info *drc,
				void *data,
				int *ret_code),
		char *opt_drc_type,
		void *data)
{
	struct property *info;
	unsigned int entries;
	struct of_drc_info drc;
	const __be32 *value;
	int j, ret_code = -EINVAL;
	bool done = false;

	info = of_find_property(dn, "ibm,drc-info", NULL);
	if (info == NULL)
		return -EINVAL;

	value = info->value;
	entries = of_read_number(value++, 1);

	for (j = 0, done = 0; (j < entries) && (!done); j++) {
		of_read_drc_info_cell(&info, &value, &drc);

		if (opt_drc_type && strcmp(opt_drc_type, drc.drc_type))
			continue;

		done = usercb(&drc, data, &ret_code);
	}

	return ret_code;
}

static int get_children_props(struct device_node *dn, const int **drc_indexes,
		const int **drc_names, const int **drc_types,
		const int **drc_power_domains)
{
	const int *indexes, *names, *types, *domains;

	indexes = of_get_property(dn, "ibm,drc-indexes", NULL);
	names = of_get_property(dn, "ibm,drc-names", NULL);
	types = of_get_property(dn, "ibm,drc-types", NULL);
	domains = of_get_property(dn, "ibm,drc-power-domains", NULL);

	if (!indexes || !names || !types || !domains) {
		/* Slot does not have dynamically-removable children */
		return -EINVAL;
	}
	if (drc_indexes)
		*drc_indexes = indexes;
	if (drc_names)
		/* &drc_names[1] contains NULL terminated slot names */
		*drc_names = names;
	if (drc_types)
		/* &drc_types[1] contains NULL terminated slot types */
		*drc_types = types;
	if (drc_power_domains)
		*drc_power_domains = domains;

	return 0;
}

static int is_php_type(char *drc_type)
{
	unsigned long value;
	char *endptr;

	/* PCI Hotplug nodes have an integer for drc_type */
	value = simple_strtoul(drc_type, &endptr, 10);
	if (endptr == drc_type)
		return 0;

	return 1;
}

/**
 * is_php_dn() - return 1 if this is a hotpluggable pci slot, else 0
 * @dn: target &device_node
 * @indexes: passed to get_children_props()
 * @names: passed to get_children_props()
 * @types: returned from get_children_props()
 * @power_domains:
 *
 * This routine will return true only if the device node is
 * a hotpluggable slot. This routine will return false
 * for built-in pci slots (even when the built-in slots are
 * dlparable.)
 */
static int is_php_dn(struct device_node *dn, const int **indexes,
		const int **names, const int **types,
		const int **power_domains)
{
	const int *drc_types;
	int rc;

	rc = get_children_props(dn, indexes, names, &drc_types,
				power_domains);
	if (rc < 0)
		return 0;

	if (!is_php_type((char *) &drc_types[1]))
		return 0;

	*types = drc_types;
	return 1;
}

struct find_drc_match_cb_struct {
	struct device_node *dn;
	bool (*usercb)(struct device_node *dn,
			u32 drc_index, char *drc_name,
			char *drc_type, u32 drc_power_domain,
			void *data);
	char *drc_type;
	char *drc_name;
	u32 drc_index;
	bool match_drc_index;
	bool add_slot;
	void *data;
};

static int find_drc_match_v1(struct device_node *dn, void *data)
{
	struct find_drc_match_cb_struct *cdata = data;
	int i, retval = 0;
	const int *indexes, *names, *types, *domains;
	char *name, *type;
	struct device_node *root = dn;

	if (cdata->match_drc_index)
		root = dn->parent;

	if (cdata->add_slot) {
		/* If this is not a hotplug slot, return without doing
		 * anything.
		 */
		if (!is_php_dn(root, &indexes, &names, &types, &domains))
			return 0;
	} else {
		if (get_children_props(root, &indexes, &names, &types,
			&domains) < 0)
			return 0;
	}

	dbg("Entry %s: dn=%pOF\n", __func__, dn);

	name = (char *) &names[1];
	type = (char *) &types[1];
	for (i = 0; i < be32_to_cpu(indexes[0]); i++) {

		if (cdata->match_drc_index &&
			((unsigned int) indexes[i + 1] != cdata->drc_index)) {
			name += strlen(name) + 1;
			type += strlen(type) + 1;
			continue;
		}

		if (((cdata->drc_name == NULL) ||
		     (cdata->drc_name && !strcmp(cdata->drc_name, name))) &&
		    ((cdata->drc_type == NULL) ||
		     (cdata->drc_type && !strcmp(cdata->drc_type, type)))) {

			if (cdata->usercb) {
				retval = cdata->usercb(dn,
					be32_to_cpu(indexes[i + 1]),
					name, type,
					be32_to_cpu(domains[i + 1]),
					cdata->data);
				if (!retval)
					return retval;
			} else {
				return 0;
			}
		}

		name += strlen(name) + 1;
		type += strlen(type) + 1;
	}

	dbg("%s - Exit: rc[%d]\n", __func__, retval);

	/* XXX FIXME: reports a failure only if last entry in loop failed */
	return retval;
}

static bool find_drc_match_v2_cb(struct of_drc_info *drc, void *data,
				int *ret_code)
{
	struct find_drc_match_cb_struct *cdata = data;
	u32 drc_index;
	char drc_name[MAX_DRC_NAME_LEN];
	int i, retval;

	(*ret_code) = -EINVAL;

	/* This set not a PHP type? */
	if (cdata->add_slot) {
		if (!is_php_type((char *) drc->drc_type)) {
			return false;
		}
	}

	/* Anything to use from this set? */
	if (cdata->match_drc_index && (cdata->drc_index > drc->last_drc_index))
		return false;
	if ((cdata->drc_type && strcmp(cdata->drc_type, drc->drc_type))
		return false;

	/* Check the drc-index entries of this set */
	for (i = 0, drc_index = drc->drc_index_start;
		i < drc->num_sequential_elems; i++, drc_index++) {

		if (cdata->match_drc_index && (cdata->drc_index != drc_index))
			continue;

		sprintf(drc_name, "%s%d", drc->drc_name_prefix,
			drc_index - drc->drc_index_start +
			drc->drc_name_suffix_start);

		if ((cdata->drc_name == NULL) ||
		    (cdata->drc_name && !strcmp(cdata->drc_name, drc_name))) {

			if (cdata->usercb) {
				retval = cdata->usercb(cdata->dn,
						drc_index, drc_name,
						drc->drc_type,
						drc->drc_power_domain,
						cdata->data);
				if (!retval) {
					(*ret_code) = retval;
					return true;
				}
			} else {
				(*ret_code) = 0;
				return true;
			}
		}
	}

	(*ret_code) = retval;
	return false;
}

static int find_drc_match_v2(struct device_node *dn, void *data)
{
	struct find_drc_match_cb_struct *cdata = data;
	struct device_node *root = cdata->dn;

	if (!cdata->add_slot) {
		if (!cdata->drc_type ||
			(cdata->drc_type && strcmp(cdata->drc_type, "SLOT")))
			root = dn->parent;
	}

	return walk_drc_info(root, find_drc_match_v2_cb, NULL, data);
}

int arch_find_drc_match(struct device_node *dn,
			bool (*usercb)(struct device_node *dn,
				u32 drc_index, char *drc_name,
				char *drc_type, u32 drc_power_domain,
				void *data),
			char *opt_drc_type, char *opt_drc_name,
			bool match_drc_index, bool add_slot,
			void *data)
{
	struct find_drc_match_cb_struct cdata = { dn, usercb,
			opt_drc_type, opt_drc_name, 0,
			match_drc_index, add_slot, data };

	if (match_drc_index) {
		const int *my_index =
			of_get_property(dn, "ibm,my-drc-index", NULL);
		if (!my_index) {
			/* Node isn't DLPAR/hotplug capable */
			return -EINVAL;
		}
		cdata.drc_index = *my_index;
	}

	if (firmware_has_feature(FW_FEATURE_DRC_INFO))
		return find_drc_match_v2(dn, &cdata);
	else
		return find_drc_match_v1(dn, &cdata);
}
EXPORT_SYMBOL(arch_find_drc_match);
