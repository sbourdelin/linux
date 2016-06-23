/*
 * Capability cgroup
 *
 * Copyright 2016 Topi Miettinen
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License.  See the file COPYING in the main directory of the
 * Linux distribution for more details.
 */

#include <linux/capability.h>
#include <linux/capability_cgroup.h>
#include <linux/cgroup.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

static DEFINE_MUTEX(capcg_mutex);

struct capcg_cgroup {
	struct cgroup_subsys_state css;
	kernel_cap_t cap_used; /* Capabilities actually used */
};

static inline struct capcg_cgroup *css_to_capcg(struct cgroup_subsys_state *s)
{
	return s ? container_of(s, struct capcg_cgroup, css) : NULL;
}

static inline struct capcg_cgroup *task_to_capcg(struct task_struct *task)
{
	return css_to_capcg(task_css(task, capability_cgrp_id));
}

static struct cgroup_subsys_state *capcg_css_alloc(struct cgroup_subsys_state
						   *parent)
{
	struct capcg_cgroup *caps;

	caps = kzalloc(sizeof(*caps), GFP_KERNEL);
	if (!caps)
		return ERR_PTR(-ENOMEM);

	cap_clear(caps->cap_used);
	return &caps->css;
}

static void capcg_css_free(struct cgroup_subsys_state *css)
{
	kfree(css_to_capcg(css));
}

static int capcg_seq_show_used(struct seq_file *m, void *v)
{
	struct capcg_cgroup *capcg = css_to_capcg(seq_css(m));
	struct cgroup_subsys_state *pos;
	u32 capi;
	kernel_cap_t subsys_caps = capcg->cap_used;

	rcu_read_lock();

	css_for_each_child(pos, &capcg->css) {
		struct capcg_cgroup *pos_capcg = css_to_capcg(pos);

		subsys_caps = cap_combine(subsys_caps, pos_capcg->cap_used);
	}

	rcu_read_unlock();

	CAP_FOR_EACH_U32(capi) {
		seq_printf(m, "%08x",
			   subsys_caps.cap[CAP_LAST_U32 - capi]);
	}
	seq_putc(m, '\n');

	return 0;
}

static struct cftype capcg_files[] = {
	{
		.name = "used",
		.seq_show = capcg_seq_show_used,
	},
	{ }	/* terminate */
};

struct cgroup_subsys capability_cgrp_subsys = {
	.css_alloc = capcg_css_alloc,
	.css_free = capcg_css_free,
	.dfl_cftypes = capcg_files,
};

void capability_cgroup_update_used(int cap)
{
	struct capcg_cgroup *caps = task_to_capcg(current);

	mutex_lock(&capcg_mutex);
	cap_raise(caps->cap_used, cap);
	mutex_unlock(&capcg_mutex);
}
