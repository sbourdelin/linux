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

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/livepatch.h>

static DEFINE_HASHTABLE(klp_shadow_hash, 12);
static DEFINE_SPINLOCK(klp_shadow_lock);

struct klp_shadow {
	struct hlist_node node;
	struct rcu_head rcu_head;
	void *obj;
	char *var;
	void *data;
};

void *klp_shadow_attach(void *obj, char *var, gfp_t gfp, void *data)
{
	unsigned long flags;
	struct klp_shadow *shadow;

	shadow = kmalloc(sizeof(*shadow), gfp);
	if (!shadow)
		return NULL;

	shadow->obj = obj;

	shadow->var = kstrdup(var, gfp);
	if (!shadow->var) {
		kfree(shadow);
		return NULL;
	}

	shadow->data = data;

	spin_lock_irqsave(&klp_shadow_lock, flags);
	hash_add_rcu(klp_shadow_hash, &shadow->node, (unsigned long)obj);
	spin_unlock_irqrestore(&klp_shadow_lock, flags);

	return shadow->data;
}
EXPORT_SYMBOL_GPL(klp_shadow_attach);

static void klp_shadow_rcu_free(struct rcu_head *head)
{
	struct klp_shadow *shadow;

	shadow = container_of(head, struct klp_shadow, rcu_head);

	kfree(shadow->var);
	kfree(shadow);
}

void klp_shadow_detach(void *obj, char *var)
{
	unsigned long flags;
	struct klp_shadow *shadow;

	spin_lock_irqsave(&klp_shadow_lock, flags);

	hash_for_each_possible(klp_shadow_hash, shadow, node,
			       (unsigned long)obj) {
		if (shadow->obj == obj && !strcmp(shadow->var, var)) {
			hash_del_rcu(&shadow->node);
			spin_unlock_irqrestore(&klp_shadow_lock, flags);
			call_rcu(&shadow->rcu_head, klp_shadow_rcu_free);
			return;
		}
	}

	spin_unlock_irqrestore(&klp_shadow_lock, flags);
}
EXPORT_SYMBOL_GPL(klp_shadow_detach);

void *klp_shadow_get(void *obj, char *var)
{
	struct klp_shadow *shadow;

	rcu_read_lock();

	hash_for_each_possible_rcu(klp_shadow_hash, shadow, node,
				   (unsigned long)obj) {
		if (shadow->obj == obj && !strcmp(shadow->var, var)) {
			rcu_read_unlock();
			return shadow->data;
		}
	}

	rcu_read_unlock();

	return NULL;
}
EXPORT_SYMBOL_GPL(klp_shadow_get);
