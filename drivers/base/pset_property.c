// SPDX-License-Identifier: GPL-2.0
/*
 * Handling of device properties defined in legacy board files.
 *
 * Copyright (C) 2014, Intel Corporation
 * Authors: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 *          Mika Westerberg <mika.westerberg@linux.intel.com>
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/property.h>
#include <linux/slab.h>

struct property_set {
	struct device *dev;
	struct fwnode_handle fwnode;
	const struct property_entry *properties;

	struct property_set *parent;
	/* Entry in parent->children list */
	struct list_head child_node;
	struct list_head children;
};

static const struct fwnode_operations pset_fwnode_ops;

static inline bool is_pset_node(const struct fwnode_handle *fwnode)
{
	return !IS_ERR_OR_NULL(fwnode) && fwnode->ops == &pset_fwnode_ops;
}

#define to_pset_node(__fwnode)						\
	({								\
		typeof(__fwnode) __to_pset_node_fwnode = __fwnode;	\
									\
		is_pset_node(__to_pset_node_fwnode) ?			\
			container_of(__to_pset_node_fwnode,		\
				     struct property_set, fwnode) :	\
			NULL;						\
	})

static const struct property_entry *
pset_prop_get(const struct property_set *pset, const char *name)
{
	const struct property_entry *prop;

	if (!pset || !pset->properties)
		return NULL;

	for (prop = pset->properties; prop->name; prop++)
		if (!strcmp(name, prop->name))
			return prop;

	return NULL;
}

static const void *property_get_pointer(const struct property_entry *prop)
{
	switch (prop->type) {
	case DEV_PROP_U8:
		if (prop->is_array)
			return prop->pointer.u8_data;
		return &prop->value.u8_data;
	case DEV_PROP_U16:
		if (prop->is_array)
			return prop->pointer.u16_data;
		return &prop->value.u16_data;
	case DEV_PROP_U32:
		if (prop->is_array)
			return prop->pointer.u32_data;
		return &prop->value.u32_data;
	case DEV_PROP_U64:
		if (prop->is_array)
			return prop->pointer.u64_data;
		return &prop->value.u64_data;
	case DEV_PROP_STRING:
		if (prop->is_array)
			return prop->pointer.str;
		return &prop->value.str;
	default:
		return NULL;
	}
}

static void property_set_pointer(struct property_entry *prop, const void *pointer)
{
	switch (prop->type) {
	case DEV_PROP_U8:
		if (prop->is_array)
			prop->pointer.u8_data = pointer;
		else
			prop->value.u8_data = *((u8 *)pointer);
		break;
	case DEV_PROP_U16:
		if (prop->is_array)
			prop->pointer.u16_data = pointer;
		else
			prop->value.u16_data = *((u16 *)pointer);
		break;
	case DEV_PROP_U32:
		if (prop->is_array)
			prop->pointer.u32_data = pointer;
		else
			prop->value.u32_data = *((u32 *)pointer);
		break;
	case DEV_PROP_U64:
		if (prop->is_array)
			prop->pointer.u64_data = pointer;
		else
			prop->value.u64_data = *((u64 *)pointer);
		break;
	case DEV_PROP_STRING:
		if (prop->is_array)
			prop->pointer.str = pointer;
		else
			prop->value.str = pointer;
		break;
	default:
		break;
	}
}

static const void *pset_prop_find(const struct property_set *pset,
				  const char *propname, size_t length)
{
	const struct property_entry *prop;
	const void *pointer;

	prop = pset_prop_get(pset, propname);
	if (!prop)
		return ERR_PTR(-EINVAL);
	pointer = property_get_pointer(prop);
	if (!pointer)
		return ERR_PTR(-ENODATA);
	if (length > prop->length)
		return ERR_PTR(-EOVERFLOW);
	return pointer;
}

static int pset_prop_read_u8_array(const struct property_set *pset,
				   const char *propname,
				   u8 *values, size_t nval)
{
	const void *pointer;
	size_t length = nval * sizeof(*values);

	pointer = pset_prop_find(pset, propname, length);
	if (IS_ERR(pointer))
		return PTR_ERR(pointer);

	memcpy(values, pointer, length);
	return 0;
}

static int pset_prop_read_u16_array(const struct property_set *pset,
				    const char *propname,
				    u16 *values, size_t nval)
{
	const void *pointer;
	size_t length = nval * sizeof(*values);

	pointer = pset_prop_find(pset, propname, length);
	if (IS_ERR(pointer))
		return PTR_ERR(pointer);

	memcpy(values, pointer, length);
	return 0;
}

static int pset_prop_read_u32_array(const struct property_set *pset,
				    const char *propname,
				    u32 *values, size_t nval)
{
	const void *pointer;
	size_t length = nval * sizeof(*values);

	pointer = pset_prop_find(pset, propname, length);
	if (IS_ERR(pointer))
		return PTR_ERR(pointer);

	memcpy(values, pointer, length);
	return 0;
}

static int pset_prop_read_u64_array(const struct property_set *pset,
				    const char *propname,
				    u64 *values, size_t nval)
{
	const void *pointer;
	size_t length = nval * sizeof(*values);

	pointer = pset_prop_find(pset, propname, length);
	if (IS_ERR(pointer))
		return PTR_ERR(pointer);

	memcpy(values, pointer, length);
	return 0;
}

static int pset_prop_count_elems_of_size(const struct property_set *pset,
					 const char *propname, size_t length)
{
	const struct property_entry *prop;

	prop = pset_prop_get(pset, propname);
	if (!prop)
		return -EINVAL;

	return prop->length / length;
}

static int pset_prop_read_string_array(const struct property_set *pset,
				       const char *propname,
				       const char **strings, size_t nval)
{
	const struct property_entry *prop;
	const void *pointer;
	size_t array_len, length;

	/* Find out the array length. */
	prop = pset_prop_get(pset, propname);
	if (!prop)
		return -EINVAL;

	if (!prop->is_array)
		/* The array length for a non-array string property is 1. */
		array_len = 1;
	else
		/* Find the length of an array. */
		array_len = pset_prop_count_elems_of_size(pset, propname,
							  sizeof(const char *));

	/* Return how many there are if strings is NULL. */
	if (!strings)
		return array_len;

	array_len = min(nval, array_len);
	length = array_len * sizeof(*strings);

	pointer = pset_prop_find(pset, propname, length);
	if (IS_ERR(pointer))
		return PTR_ERR(pointer);

	memcpy(strings, pointer, length);

	return array_len;
}

static bool pset_fwnode_property_present(const struct fwnode_handle *fwnode,
					 const char *propname)
{
	return !!pset_prop_get(to_pset_node(fwnode), propname);
}

static int pset_fwnode_read_int_array(const struct fwnode_handle *fwnode,
				      const char *propname,
				      unsigned int elem_size, void *val,
				      size_t nval)
{
	const struct property_set *node = to_pset_node(fwnode);

	if (!val)
		return pset_prop_count_elems_of_size(node, propname, elem_size);

	switch (elem_size) {
	case sizeof(u8):
		return pset_prop_read_u8_array(node, propname, val, nval);
	case sizeof(u16):
		return pset_prop_read_u16_array(node, propname, val, nval);
	case sizeof(u32):
		return pset_prop_read_u32_array(node, propname, val, nval);
	case sizeof(u64):
		return pset_prop_read_u64_array(node, propname, val, nval);
	}

	return -ENXIO;
}

static int
pset_fwnode_property_read_string_array(const struct fwnode_handle *fwnode,
				       const char *propname,
				       const char **val, size_t nval)
{
	return pset_prop_read_string_array(to_pset_node(fwnode), propname,
					   val, nval);
}

struct fwnode_handle *pset_fwnode_get_parent(const struct fwnode_handle *fwnode)
{
	struct property_set *pset = to_pset_node(fwnode);

	return pset ? &pset->parent->fwnode : NULL;
}

struct fwnode_handle *
pset_fwnode_get_next_subnode(const struct fwnode_handle *fwnode,
			     struct fwnode_handle *child)
{
	const struct property_set *pset = to_pset_node(fwnode);
	struct property_set *first_child;
	struct property_set *next;

	if (!pset)
		return NULL;

	if (list_empty(&pset->children))
		return NULL;

	first_child = list_first_entry(&pset->children, struct property_set,
				       child_node);

	if (child) {
		next = list_next_entry(to_pset_node(child), child_node);
		if (next == first_child)
			return NULL;
	} else {
		next = first_child;
	}

	return &next->fwnode;
}

static const struct fwnode_operations pset_fwnode_ops = {
	.property_present = pset_fwnode_property_present,
	.property_read_int_array = pset_fwnode_read_int_array,
	.property_read_string_array = pset_fwnode_property_read_string_array,
	.get_parent = pset_fwnode_get_parent,
	.get_next_child_node = pset_fwnode_get_next_subnode,
};

static void property_entry_free_data(const struct property_entry *p)
{
	const void *pointer = property_get_pointer(p);
	size_t i, nval;

	if (p->is_array) {
		if (p->type == DEV_PROP_STRING && p->pointer.str) {
			nval = p->length / sizeof(const char *);
			for (i = 0; i < nval; i++)
				kfree(p->pointer.str[i]);
		}
		kfree(pointer);
	} else if (p->type == DEV_PROP_STRING) {
		kfree(p->value.str);
	}
	kfree(p->name);
}

static int property_copy_string_array(struct property_entry *dst,
				      const struct property_entry *src)
{
	const char **d;
	size_t nval = src->length / sizeof(*d);
	int i;

	d = kcalloc(nval, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	for (i = 0; i < nval; i++) {
		d[i] = kstrdup(src->pointer.str[i], GFP_KERNEL);
		if (!d[i] && src->pointer.str[i]) {
			while (--i >= 0)
				kfree(d[i]);
			kfree(d);
			return -ENOMEM;
		}
	}

	dst->pointer.str = d;
	return 0;
}

static int property_entry_copy_data(struct property_entry *dst,
				    const struct property_entry *src)
{
	const void *pointer = property_get_pointer(src);
	const void *new;
	int error;

	if (src->is_array) {
		if (!src->length)
			return -ENODATA;

		if (src->type == DEV_PROP_STRING) {
			error = property_copy_string_array(dst, src);
			if (error)
				return error;
			new = dst->pointer.str;
		} else {
			new = kmemdup(pointer, src->length, GFP_KERNEL);
			if (!new)
				return -ENOMEM;
		}
	} else if (src->type == DEV_PROP_STRING) {
		new = kstrdup(src->value.str, GFP_KERNEL);
		if (!new && src->value.str)
			return -ENOMEM;
	} else {
		new = pointer;
	}

	dst->length = src->length;
	dst->is_array = src->is_array;
	dst->type = src->type;

	property_set_pointer(dst, new);

	dst->name = kstrdup(src->name, GFP_KERNEL);
	if (!dst->name)
		goto out_free_data;

	return 0;

out_free_data:
	property_entry_free_data(dst);
	return -ENOMEM;
}

/**
 * property_entries_dup - duplicate array of properties
 * @properties: array of properties to copy
 *
 * This function creates a deep copy of the given NULL-terminated array
 * of property entries.
 */
struct property_entry *
property_entries_dup(const struct property_entry *properties)
{
	struct property_entry *p;
	int i, n = 0;

	while (properties[n].name)
		n++;

	p = kcalloc(n + 1, sizeof(*p), GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < n; i++) {
		int ret = property_entry_copy_data(&p[i], &properties[i]);
		if (ret) {
			while (--i >= 0)
				property_entry_free_data(&p[i]);
			kfree(p);
			return ERR_PTR(ret);
		}
	}

	return p;
}
EXPORT_SYMBOL_GPL(property_entries_dup);

/**
 * property_entries_free - free previously allocated array of properties
 * @properties: array of properties to destroy
 *
 * This function frees given NULL-terminated array of property entries,
 * along with their data.
 */
void property_entries_free(const struct property_entry *properties)
{
	const struct property_entry *p;

	for (p = properties; p->name; p++)
		property_entry_free_data(p);

	kfree(properties);
}
EXPORT_SYMBOL_GPL(property_entries_free);

/**
 * pset_free_set - releases memory allocated for copied property set
 * @pset: Property set to release
 *
 * Function takes previously copied property set and releases all the
 * memory allocated to it.
 */
static void pset_free_set(struct property_set *pset)
{
	struct property_set *child, *next;

	if (!pset)
		return;

	list_for_each_entry_safe(child, next, &pset->children, child_node) {
		list_del(&child->child_node);
		pset_free_set(child);
	}

	property_entries_free(pset->properties);
	kfree(pset);
}

/**
 * pset_create_set - creates property set.
 * @src: property entries for the set.
 *
 * This function takes a deep copy of the given property entries and creates
 * property set. Call pset_free_set() to free resources allocated in this
 * function.
 *
 * Return: Pointer to the new property set or error pointer.
 */
static struct property_set *pset_create_set(const struct property_entry *src)
{
	struct property_entry *properties;
	struct property_set *p;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&p->child_node);
	INIT_LIST_HEAD(&p->children);
	p->fwnode.ops = &pset_fwnode_ops;

	properties = property_entries_dup(src);
	if (IS_ERR(properties)) {
		kfree(p);
		return ERR_CAST(properties);
	}

	p->properties = properties;
	return p;
}

/**
 * device_remove_properties - Remove properties from a device object.
 * @dev: Device whose properties to remove.
 *
 * The function removes properties previously associated to the device
 * secondary firmware node with device_add_properties(). Memory allocated
 * to the properties will also be released.
 */
void device_remove_properties(struct device *dev)
{
	struct fwnode_handle *fwnode;
	struct property_set *pset;

	fwnode = dev_fwnode(dev);
	if (!fwnode)
		return;
	/*
	 * Pick either primary or secondary node depending which one holds
	 * the pset. If there is no real firmware node (ACPI/DT) primary
	 * will hold the pset.
	 */
	pset = to_pset_node(fwnode);
	if (pset) {
		set_primary_fwnode(dev, NULL);
	} else {
		pset = to_pset_node(fwnode->secondary);
		if (pset && dev == pset->dev)
			set_secondary_fwnode(dev, NULL);
	}
	if (pset && dev == pset->dev)
		pset_free_set(pset);
}
EXPORT_SYMBOL_GPL(device_remove_properties);

/**
 * device_add_properties - Add a collection of properties to a device object.
 * @dev: Device to add properties to.
 * @properties: Collection of properties to add.
 *
 * Associate a collection of device properties represented by @properties with
 * @dev as its secondary firmware node. The function takes a copy of
 * @properties.
 */
int device_add_properties(struct device *dev,
			  const struct property_entry *properties)
{
	struct property_set *p;

	if (!properties)
		return -EINVAL;

	p = pset_create_set(properties);
	if (IS_ERR(p))
		return PTR_ERR(p);

	set_secondary_fwnode(dev, &p->fwnode);
	p->dev = dev;
	return 0;
}
EXPORT_SYMBOL_GPL(device_add_properties);

/**
 * device_add_child_properties - Add a collection of properties to a device object.
 * @dev: Device to add properties to.
 * @properties: Collection of properties to add.
 *
 * Associate a collection of device properties represented by @properties as a
 * child of given @parent firmware node.  The function takes a copy of
 * @properties.
 */
struct fwnode_handle *
device_add_child_properties(struct device *dev,
			    struct fwnode_handle *parent,
			    const struct property_entry *properties)
{
	struct property_set *p;
	struct property_set *parent_pset;

	if (!properties)
		return ERR_PTR(-EINVAL);

	parent_pset = to_pset_node(parent);
	if (!parent_pset)
		return ERR_PTR(-EINVAL);

	p = pset_create_set(properties);
	if (IS_ERR(p))
		return ERR_CAST(p);

	p->dev = dev;
	p->parent = parent_pset;
	list_add_tail(&p->child_node, &parent_pset->children);

	return &p->fwnode;
}
EXPORT_SYMBOL_GPL(device_add_child_properties);
