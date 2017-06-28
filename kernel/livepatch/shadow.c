/*
 * shadow.c - Shadow Variables
 *
 * Copyright (C) 2014 Josh Poimboeuf <jpoimboe@redhat.com>
 * Copyright (C) 2014 Seth Jennings <sjenning@redhat.com>
 * Copyright (C) 2017 Joe Lawrence <joe.lawrence@redhat.com>
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

/**
 * DOC: Shadow variable API concurrency notes:
 *
 * The shadow variable API simply provides a relationship between an
 * <obj, num> pair and a pointer value.  It is the responsibility of the
 * caller to provide any mutual exclusion required of the shadow data.
 *
 * Once klp_shadow_attach() adds a shadow variable to the
 * klp_shadow_hash, it is considered live and klp_shadow_get() may
 * return the shadow variable's new_data pointer.  Therefore,
 * initialization of shadow new_data should be completed before
 * attaching the shadow variable.
 *
 * Alternatively, the klp_shadow_get_or_attach() call may be used to
 * safely fetch any existing <obj, num> match, or create a new
 * <obj, num> shadow variable if none exists.
 *
 * If the API is called under a special context (like spinlocks), set
 * the GFP flags passed to klp_shadow_attach() accordingly.
 *
 * The klp_shadow_hash is an RCU-enabled hashtable and should be safe
 * against concurrent klp_shadow_detach() and klp_shadow_get()
 * operations.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/livepatch.h>

static DEFINE_HASHTABLE(klp_shadow_hash, 12);
static DEFINE_SPINLOCK(klp_shadow_lock);

/**
 * struct klp_shadow - shadow variable structure
 * @node:	klp_shadow_hash hash table node
 * @rcu_head:	RCU is used to safely free this structure
 * @obj:	pointer to original data
 * @num:	numerical description of new data
 * @new_data:	new data area
 */
struct klp_shadow {
	struct hlist_node node;
	struct rcu_head rcu_head;
	void *obj;
	unsigned long num;
	char new_data[];
};

/**
 * shadow_match() - verify a shadow variable matches given <obj, num>
 * @shadow:	shadow variable to match
 * @obj:	pointer to original data
 * @num:	numerical description of new data
 *
 * Return: true if the shadow variable matches.
 */
static inline bool shadow_match(struct klp_shadow *shadow, void *obj,
				unsigned long num)
{
	return shadow->obj == obj && shadow->num == num;
}

/**
 * klp_shadow_get() - retrieve a shadow variable new_data pointer
 * @obj:	pointer to original data
 * @num:	numerical description of new data
 *
 * Return: a pointer to shadow variable new data
 */
void *klp_shadow_get(void *obj, unsigned long num)
{
	struct klp_shadow *shadow;

	rcu_read_lock();

	hash_for_each_possible_rcu(klp_shadow_hash, shadow, node,
				   (unsigned long)obj) {

		if (shadow_match(shadow, obj, num)) {
			rcu_read_unlock();
			return shadow->new_data;
		}
	}

	rcu_read_unlock();

	return NULL;
}
EXPORT_SYMBOL_GPL(klp_shadow_get);

/**
 * _klp_shadow_attach() - allocate and add a new shadow variable
 * @obj:	pointer to original data
 * @num:	numerical description of new data
 * @new_data:	pointer to new data
 * @new_size:	size of new data
 * @gfp_flags:	GFP mask for allocation
 * @lock:	take klp_shadow_lock during klp_shadow_hash operations
 *
 * Note: allocates @new_size space for shadow variable data and copies
 * @new_size bytes from @new_data into the shadow varaible's own @new_data
 * space.  If @new_data is NULL, @new_size is still allocated, but no
 * copy is performed.
 *
 * Return: the shadow variable new_data element, NULL on failure.
 */
static void *_klp_shadow_attach(void *obj, unsigned long num, void *new_data,
				size_t new_size, gfp_t gfp_flags,
				bool lock)
{
	struct klp_shadow *shadow;
	unsigned long flags;

	shadow = kzalloc(new_size + sizeof(*shadow), gfp_flags);
	if (!shadow)
		return NULL;

	shadow->obj = obj;
	shadow->num = num;
	if (new_data)
		memcpy(shadow->new_data, new_data, new_size);

	if (lock)
		spin_lock_irqsave(&klp_shadow_lock, flags);
	hash_add_rcu(klp_shadow_hash, &shadow->node, (unsigned long)obj);
	if (lock)
		spin_unlock_irqrestore(&klp_shadow_lock, flags);

	return shadow->new_data;
}

/**
 * klp_shadow_attach() - allocate and add a new shadow variable
 * @obj:	pointer to original data
 * @num:	numerical description of new num
 * @new_data:	pointer to new data
 * @new_size:	size of new data
 * @gfp_flags:	GFP mask for allocation
 *
 * Return: the shadow variable new_data element, NULL on failure.
 */
void *klp_shadow_attach(void *obj, unsigned long num, void *new_data,
			size_t new_size, gfp_t gfp_flags)
{
	return _klp_shadow_attach(obj, num, new_data, new_size,
				  gfp_flags, true);
}
EXPORT_SYMBOL_GPL(klp_shadow_attach);

/**
 * klp_shadow_get_or_attach() - get existing or attach a new shadow variable
 * @obj:	pointer to original data
 * @num:	numerical description of new data
 * @new_data:	pointer to new data
 * @new_size:   size of new data
 * @gfp_flags:	GFP mask used to allocate shadow variable metadata
 *
 * Note: if memory allocation is necessary, it will do so under a spinlock,
 * so @gfp_flags should include GFP_NOWAIT, or GFP_ATOMIC, etc.
 *
 * Return: the shadow variable new_data element, NULL on failure.
 */
void *klp_shadow_get_or_attach(void *obj, unsigned long num, void *new_data,
			       size_t new_size, gfp_t gfp_flags)
{
	void *nd;
	unsigned long flags;

	nd = klp_shadow_get(obj, num);

	if (!nd) {
		spin_lock_irqsave(&klp_shadow_lock, flags);
		nd = klp_shadow_get(obj, num);
		if (!nd)
			nd = _klp_shadow_attach(obj, num, new_data, new_size,
						gfp_flags, false);
		spin_unlock_irqrestore(&klp_shadow_lock, flags);
	}

	return nd;

}
EXPORT_SYMBOL_GPL(klp_shadow_get_or_attach);

/**
 * klp_shadow_detach() - detach and free a <obj, num> shadow variable
 * @obj:	pointer to original data
 * @num:	numerical description of new data
 */
void klp_shadow_detach(void *obj, unsigned long num)
{
	struct klp_shadow *shadow;
	unsigned long flags;

	spin_lock_irqsave(&klp_shadow_lock, flags);

	/* Delete all <obj, num> from hash */
	hash_for_each_possible(klp_shadow_hash, shadow, node,
			       (unsigned long)obj) {

		if (shadow_match(shadow, obj, num)) {
			hash_del_rcu(&shadow->node);
			kfree_rcu(shadow, rcu_head);
			break;
		}
	}

	spin_unlock_irqrestore(&klp_shadow_lock, flags);
}
EXPORT_SYMBOL_GPL(klp_shadow_detach);

/**
 * klp_shadow_detach_all() - detach all <*, num> shadow variables
 * @num:	numerical description of new data
 */
void klp_shadow_detach_all(unsigned long num)
{
	struct klp_shadow *shadow;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&klp_shadow_lock, flags);

	/* Delete all <*, num> from hash */
	hash_for_each(klp_shadow_hash, i, shadow, node) {
		if (shadow_match(shadow, shadow->obj, num)) {
			hash_del_rcu(&shadow->node);
			kfree_rcu(shadow, rcu_head);
		}
	}

	spin_unlock_irqrestore(&klp_shadow_lock, flags);
}
EXPORT_SYMBOL_GPL(klp_shadow_detach_all);
