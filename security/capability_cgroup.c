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
#include <linux/cred.h>
#include <linux/security.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

static DEFINE_MUTEX(capcg_mutex);

struct capcg_cgroup {
	struct cgroup_subsys_state css;
	kernel_cap_t cap_bset; /* Capability bounding set */
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

	caps->cap_bset = CAP_FULL_SET;
	cap_clear(caps->cap_used);
	return &caps->css;
}

static void capcg_css_free(struct cgroup_subsys_state *css)
{
	kfree(css_to_capcg(css));
}

/**
 * capcg_apply_bset - apply cgroup bounding set to all task's capabilities
 */
static int capcg_task_apply_bset(struct task_struct *task, kernel_cap_t bset)
{
	struct cred *new;
	const struct cred *old;
	kernel_cap_t bounding, effective, inheritable, permitted;
	int ret;

	new = prepare_creds();
	if (!new)
		return -ENOMEM;

	ret = security_capget(task, 
			      &effective, &inheritable, &permitted);
	if (ret < 0)
		goto abort_cred;

	old = get_task_cred(task);
	bounding = cap_intersect(bset, old->cap_bset);
	effective = cap_intersect(bset, effective);
	inheritable = cap_intersect(bset, inheritable);
	permitted = cap_intersect(bset, permitted);

	/* security_capset() also updates ambient capabilities */
	ret = security_capset(new, old,
			      &effective, &inheritable, &permitted);
	new->cap_bset = bounding;
		
	put_cred(old);
	if (ret < 0)
		goto abort_cred;

	ret = commit_creds(new);
	return ret;

 abort_cred:
	abort_creds(new);
	return ret;
}

static void capcg_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *css;

	rcu_read_lock();
	cgroup_taskset_for_each(task, css, tset) {
		struct capcg_cgroup *caps = css_to_capcg(css);
		
		capcg_task_apply_bset(task, caps->cap_bset);
	}
	rcu_read_unlock();
}

/** capcg_write_bset - update css tree and their tasks with new
 *  bounding capability
 */
static ssize_t capcg_write_bset(struct kernfs_open_file *of, char *buf,
				size_t nbytes, loff_t off)
{
	struct cgroup_subsys_state *css = of_css(of), *pos;
	struct capcg_cgroup *caps = css_to_capcg(css);
	u32 capi;
	int err;
	kernel_cap_t new_bset;

	buf = strstrip(buf);

	CAP_FOR_EACH_U32(capi) {
		char buf2[9]; /* for each 32 bit block */
		u32 capv;

		memcpy(buf2, &buf[capi * 8], 8);
		buf2[8] = '\0';
		err = kstrtou32(buf2, 16, &capv);
		if (err)
			return err;
		new_bset.cap[CAP_LAST_U32 - capi] = capv;
	}

	mutex_lock(&capcg_mutex);
	caps->cap_bset = cap_intersect(caps->cap_bset, new_bset);
	mutex_unlock(&capcg_mutex);

	rcu_read_lock();
	css_for_each_child(pos, css) {
		struct css_task_iter it;
		struct task_struct *task;

		css_task_iter_start(pos, &it);
		while ((task = css_task_iter_next(&it)))
			capcg_task_apply_bset(task, new_bset);
	}
	rcu_read_unlock();

	return nbytes;
}

static int capcg_seq_show_cap(struct seq_file *m, kernel_cap_t *cap)
{
	u32 capi;

	rcu_read_lock();

	CAP_FOR_EACH_U32(capi) {
		seq_printf(m, "%08x",
			   cap->cap[CAP_LAST_U32 - capi]);
	}
	seq_putc(m, '\n');

	rcu_read_unlock();

	return 0;
}

static int capcg_seq_show_bset(struct seq_file *m, void *v)
{
	struct capcg_cgroup *capcg = css_to_capcg(seq_css(m));

	return capcg_seq_show_cap(m, &capcg->cap_bset);
}

static int capcg_seq_show_used(struct seq_file *m, void *v)
{
	struct capcg_cgroup *capcg = css_to_capcg(seq_css(m));

	return capcg_seq_show_cap(m, &capcg->cap_used);
}

static struct cftype capcg_files[] = {
	{
		.name = "bounding_set",
		.seq_show = capcg_seq_show_bset,
		.write = capcg_write_bset,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "used",
		.seq_show = capcg_seq_show_used,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{ }	/* terminate */
};

struct cgroup_subsys capability_cgrp_subsys = {
	.css_alloc = capcg_css_alloc,
	.css_free = capcg_css_free,
	.attach = capcg_attach,
	.dfl_cftypes = capcg_files,
};

void capability_cgroup_update_used(int cap)
{
	struct capcg_cgroup *caps = task_to_capcg(current);

	mutex_lock(&capcg_mutex);
	cap_raise(caps->cap_used, cap);
	mutex_unlock(&capcg_mutex);
}
