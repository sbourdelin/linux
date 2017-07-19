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
#include <linux/completion.h>
#include <linux/list.h>

#if IS_ENABLED(CONFIG_LIVEPATCH)

#include <asm/livepatch.h>

/* task patch states */
#define KLP_UNDEFINED	-1
#define KLP_UNPATCHED	 0
#define KLP_PATCHED	 1

/* livepatch mode */

extern int sysctl_livepatch_mode;
enum {
	LIVEPATCH_MODE_DEFAULT,
	LIVEPATCH_MODE_REPLACE,
};

/**
 * struct klp_func - function structure for live patching
 * @old_name:	name of the function to be patched
 * @new_func:	pointer to the patched function code
 * @old_sympos: a hint indicating which symbol position the old function
 *		can be found (optional)
 * @immediate:  patch the func immediately, bypassing safety mechanisms
 * @old_addr:	the address of the function being patched
 * @kobj:	kobject for sysfs resources
 * @stack_node:	list node for klp_ops func_stack list
 * @old_size:	size of the old function
 * @new_size:	size of the new function
 * @patched:	the func has been added to the klp_ops list
 * @transition:	the func is currently being applied or reverted
 * @no_op:	this is a no_op function used to compelete revert a function
 *
 * The patched and transition variables define the func's patching state.  When
 * patching, a func is always in one of the following states:
 *
 *   patched=0 transition=0: unpatched
 *   patched=0 transition=1: unpatched, temporary starting state
 *   patched=1 transition=1: patched, may be visible to some tasks
 *   patched=1 transition=0: patched, visible to all tasks
 *
 * And when unpatching, it goes in the reverse order:
 *
 *   patched=1 transition=0: patched, visible to all tasks
 *   patched=1 transition=1: patched, may be visible to some tasks
 *   patched=0 transition=1: unpatched, temporary ending state
 *   patched=0 transition=0: unpatched
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
	bool immediate;

	/* internal */
	unsigned long old_addr;
	struct kobject kobj;
	struct list_head stack_node;
	unsigned long old_size, new_size;
	bool patched;
	bool transition;
	bool no_op;
};

/**
 * struct klp_func_no_op - internal object used to link no_op functions, which
			   avoids the need to bloat struct klp_func
 * @orig_func:	embeds struct klp_func
 * @func_entry:	used link struct klp_func_no_op to struct klp_object
 */
struct klp_func_no_op {
	struct klp_func orig_func;
	struct list_head func_entry;
};

/**
 * struct klp_object - kernel object structure for live patching
 * @name:	module name (or NULL for vmlinux)
 * @funcs:	function entries for functions to be patched in the object
 * @kobj:	kobject for sysfs resources
 * @func_list:	head of list for struct klp_func_no_op
 * @obj_entry:	used to link struct klp_object to struct klp_patch
 * @mod:	kernel module associated with the patched object
 *		(NULL for vmlinux)
 * @patched:	the object's funcs have been added to the klp_ops list
 */
struct klp_object {
	/* external */
	const char *name;
	struct klp_func *funcs;

	/* internal */
	struct kobject kobj;
	struct list_head func_list;
	struct list_head obj_entry;
	struct module *mod;
	bool patched;
};

/**
 * struct klp_patch - patch structure for live patching
 * @mod:	reference to the live patch module
 * @objs:	object entries for kernel objects to be patched
 * @immediate:  patch all funcs immediately, bypassing safety mechanisms
 * @list:	list node for global list of registered patches
 * @kobj:	kobject for sysfs resources
 * @obj_list:	head of list for dynamically allocated struct klp_object
 * @enabled:	the patch is enabled (but operation may be incomplete)
 * @replaced:	the patch has been replaced an can not be re-enabled
 * @finish:	for waiting till it is safe to remove the patch module
 */
struct klp_patch {
	/* external */
	struct module *mod;
	struct klp_object *objs;
	bool immediate;

	/* internal */
	struct list_head list;
	struct kobject kobj;
	struct list_head obj_list;
	bool enabled;
	bool replaced;
	struct completion finish;
};

struct obj_iter {
	struct klp_object *obj;
	struct list_head *obj_list_head;
	struct list_head *obj_list_pos;
};

static inline struct klp_object *obj_iter_next(struct obj_iter *iter)
{
	struct klp_object *obj;

	if (iter->obj->funcs || iter->obj->name) {
		obj = iter->obj;
		iter->obj++;
	} else {
		if (iter->obj_list_pos == iter->obj_list_head) {
			obj = NULL;
		} else {
			obj = list_entry(iter->obj_list_pos, struct klp_object,
					 obj_entry);
			iter->obj_list_pos = iter->obj_list_pos->next;
		}
	}

	return obj;
}

static inline struct klp_object *obj_iter_init(struct klp_patch *patch,
					       struct obj_iter *iter)
{
	iter->obj = patch->objs;
	iter->obj_list_head = &patch->obj_list;
	iter->obj_list_pos = iter->obj_list_head->next;

	return obj_iter_next(iter);
}

#define klp_for_each_object(patch, obj, iter) \
	for (obj = obj_iter_init(patch, iter); obj; obj = obj_iter_next(iter))

#define klp_for_each_object_core(patch, obj) \
	for (obj = patch->objs; obj->funcs || obj->name; obj++)

struct func_iter {
	struct klp_func *func;
	struct list_head *func_list_head;
	struct list_head *func_list_pos;
};

static inline struct klp_func *func_iter_next(struct func_iter *iter)
{
	struct klp_func *func;
	struct klp_func_no_op *func_no_op;

	if (iter->func && (iter->func->old_name || iter->func->new_func ||
			   iter->func->old_sympos)) {
		func = iter->func;
		iter->func++;
	} else {
		if (iter->func_list_pos == iter->func_list_head) {
			func = NULL;
		} else {
			func_no_op = list_entry(iter->func_list_pos,
						struct klp_func_no_op,
						func_entry);
			func = &func_no_op->orig_func;
			iter->func_list_pos = iter->func_list_pos->next;
		}
	}

	return func;
}

static inline struct klp_func *func_iter_init(struct klp_object *obj,
					      struct func_iter *iter)
{
	iter->func = obj->funcs;
	iter->func_list_head = &obj->func_list;
	iter->func_list_pos = iter->func_list_head->next;

	return func_iter_next(iter);
}

#define klp_for_each_func(obj, func, iter) \
	for (func = func_iter_init(obj, iter); func; \
	     func = func_iter_next(iter))

int klp_register_patch(struct klp_patch *);
int klp_unregister_patch(struct klp_patch *);
int klp_enable_patch(struct klp_patch *);
int klp_disable_patch(struct klp_patch *);

void arch_klp_init_object_loaded(struct klp_patch *patch,
				 struct klp_object *obj);

/* Called from the module loader during module coming/going states */
int klp_module_coming(struct module *mod);
void klp_module_going(struct module *mod);

void klp_copy_process(struct task_struct *child);
void klp_update_patch_state(struct task_struct *task);

static inline bool klp_patch_pending(struct task_struct *task)
{
	return test_tsk_thread_flag(task, TIF_PATCH_PENDING);
}

static inline bool klp_have_reliable_stack(void)
{
	return IS_ENABLED(CONFIG_STACKTRACE) &&
	       IS_ENABLED(CONFIG_HAVE_RELIABLE_STACKTRACE);
}

#else /* !CONFIG_LIVEPATCH */

static inline int klp_module_coming(struct module *mod) { return 0; }
static inline void klp_module_going(struct module *mod) {}
static inline bool klp_patch_pending(struct task_struct *task) { return false; }
static inline void klp_update_patch_state(struct task_struct *task) {}
static inline void klp_copy_process(struct task_struct *child) {}

#endif /* CONFIG_LIVEPATCH */

#endif /* _LINUX_LIVEPATCH_H_ */
