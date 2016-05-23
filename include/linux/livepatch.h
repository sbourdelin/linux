/*
 * livepatch.h - Kernel Live Patching Core
 *
 * Copyright (C) 2014 Seth Jennings <sjenning@redhat.com>
 * Copyright (C) 2014 SUSE
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _LINUX_LIVEPATCH_H_
#define _LINUX_LIVEPATCH_H_

#include <linux/module.h>
#include <linux/ftrace.h>

#if IS_ENABLED(CONFIG_LIVEPATCH)

#include <asm/livepatch.h>

enum klp_state {
	KLP_DISABLED,
	KLP_ENABLED
};

/**
 * struct klp_func - function structure for live patching
 * @old_name:	name of the function to be patched
 * @new_func:	pointer to the patched function code
 * @old_sympos: a hint indicating which symbol position the old function
 *		can be found (optional)
 * @old_addr:	the address of the function being patched
 * @list:	list node for the list of patched functions in an object
 * @kobj:	kobject for sysfs resources
 * @state:	tracks function-level patch application state
 * @stack_node:	list node for klp_ops func_stack list
 */
struct klp_func {
	/* external */
	const char *old_name;
	void *new_func;
	/*
	 * The old_sympos field is optional and can be used to resolve
	 * duplicate symbol names in livepatch objects. If this field is zero,
	 * it is expected the symbol is unique, otherwise patching fails. If
	 * this value is greater than zero then that occurrence of the symbol
	 * in kallsyms for the given object is used.
	 */
	unsigned long old_sympos;

	/* internal */
	unsigned long old_addr;
	struct list_head list;
	struct list_head stack_node;
	struct kobject kobj;
	enum klp_state state;
};

/**
 * struct klp_object - kernel object structure for live patching
 * @name:	module name (or NULL for vmlinux)
 * @funcs:	function entries for functions to be patched in the object
 * @list:	list node for the list of patched objects
 * @kobj:	kobject for sysfs resources
 * @mod:	kernel module associated with the patched object
 * 		(NULL for vmlinux)
 * @state:	tracks object-level patch application state
 */
struct klp_object {
	/* external */
	const char *name;

	/* internal */
	struct list_head funcs;
	struct list_head list;
	struct kobject kobj;
	struct module *mod;
	enum klp_state state;
};

/**
 * struct klp_patch - patch structure for live patching
 * @mod:	reference to the live patch module
 * @objs:	object entries for kernel objects to be patched
 * @list:	list node for global list of registered patches
 * @kobj:	kobject for sysfs resources
 * @state:	tracks patch-level application state
 */
struct klp_patch {
	/* external */
	struct module *mod;

	/* internal */
	struct list_head objs;
	struct list_head list;
	struct kobject kobj;
	enum klp_state state;
};

struct klp_patch *klp_create_empty_patch(struct module *mod);
struct klp_object *klp_add_object(struct klp_patch *patch,
				  const char *name);
struct klp_func *klp_add_func(struct klp_object *obj,
			      const char *old_name,
			      void *new_func,
			      unsigned long old_sympos);

int klp_register_patch(struct klp_patch *);
int klp_release_patch(struct klp_patch *);
int klp_enable_patch(struct klp_patch *);
int klp_disable_patch(struct klp_patch *);

/* Called from the module loader during module coming/going states */
int klp_module_coming(struct module *mod);
void klp_module_going(struct module *mod);

#define klp_create_patch_or_die(mod)					\
({									\
	struct klp_patch *__patch;					\
									\
	__patch = klp_create_empty_patch(mod);				\
	if (IS_ERR(__patch)) {						\
		pr_err("livepatch: failed to create empty patch (%ld)\n", \
		       PTR_ERR(__patch));				\
		return PTR_ERR(__patch);				\
	}								\
	__patch;							\
})

#define klp_add_object_or_die(patch, obj_name)				\
({									\
	struct klp_object *__obj;					\
									\
	__obj = klp_add_object(patch, obj_name);			\
	if (IS_ERR(__obj)) {						\
		pr_err("livepatch: failed to add the object '%s' for the patch '%s' (%ld)\n", \
		       obj_name ? obj_name : "vmlinux",			\
		       patch->mod->name, PTR_ERR(__obj));		\
		WARN_ON(klp_release_patch(patch));			\
		return PTR_ERR(__obj);					\
	}								\
	__obj;								\
})

#define klp_add_func_or_die(patch, obj, old_name, new_func, sympos)	\
({									\
	struct klp_func *__func;					\
									\
	__func = klp_add_func(obj, old_name, new_func, sympos);		\
	if (IS_ERR(__func)) {						\
		pr_err("livepatch: failed to add the function '%s' for the object '%s' in the patch '%s' (%ld)\n", \
		       old_name ? old_name : "NULL",			\
		       obj->name ? obj->name : "vmlinux",		\
		       patch->mod->name, PTR_ERR(__func));		\
		WARN_ON(klp_release_patch(patch));			\
		return PTR_ERR(__func);					\
	}								\
	__func;								\
})

#else /* !CONFIG_LIVEPATCH */

static inline int klp_module_coming(struct module *mod) { return 0; }
static inline void klp_module_going(struct module *mod) { }

#endif /* CONFIG_LIVEPATCH */

#endif /* _LINUX_LIVEPATCH_H_ */
