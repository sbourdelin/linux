#include <linux/ctype.h>
#include <linux/mm.h>
#include <linux/slab.h>

#include "cgroup-internal.h"

static struct cgroup_subsys_state *
debug_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct cgroup_subsys_state *css = kzalloc(sizeof(*css), GFP_KERNEL);

	if (!css)
		return ERR_PTR(-ENOMEM);

	return css;
}

static void debug_css_free(struct cgroup_subsys_state *css)
{
	kfree(css);
}

/*
 * debug_taskcount_read - return the number of tasks in a cgroup.
 * @cgrp: the cgroup in question
 */
static u64 debug_taskcount_read(struct cgroup_subsys_state *css,
				struct cftype *cft)
{
	struct cgroup *cgrp = css->cgroup;
	u64 count = 0;
	struct cgrp_cset_link *link;

	spin_lock_irq(&css_set_lock);
	list_for_each_entry(link, &cgrp->cset_links, cset_link)
		count += link->cset->task_count;
	spin_unlock_irq(&css_set_lock);
	return count;
}

static int current_css_set_read(struct seq_file *seq, void *v)
{
	struct css_set *cset;
	struct cgroup_subsys *ss;
	struct cgroup_subsys_state *css;
	int i, refcnt;

	mutex_lock(&cgroup_mutex);
	spin_lock_irq(&css_set_lock);
	rcu_read_lock();
	cset = rcu_dereference(current->cgroups);
	refcnt = atomic_read(&cset->refcount);
	seq_printf(seq, "css_set %pK %d", cset, refcnt);
	if (refcnt > cset->task_count)
		seq_printf(seq, " +%d", refcnt - cset->task_count);
	seq_puts(seq, "\n");

	/*
	 * Print the css'es stored in the current css_set.
	 */
	for_each_subsys(ss, i) {
		css = cset->subsys[ss->id];
		if (!css)
			continue;
		seq_printf(seq, "%2d: %-4s\t- %lx[%d]\n", ss->id, ss->name,
			  (unsigned long)css, css->id);
	}
	rcu_read_unlock();
	spin_unlock_irq(&css_set_lock);
	mutex_unlock(&cgroup_mutex);
	return 0;
}

static u64 current_css_set_refcount_read(struct cgroup_subsys_state *css,
					 struct cftype *cft)
{
	u64 count;

	rcu_read_lock();
	count = atomic_read(&task_css_set(current)->refcount);
	rcu_read_unlock();
	return count;
}

static int current_css_set_cg_links_read(struct seq_file *seq, void *v)
{
	struct cgrp_cset_link *link;
	struct css_set *cset;
	char *name_buf;

	name_buf = kmalloc(NAME_MAX + 1, GFP_KERNEL);
	if (!name_buf)
		return -ENOMEM;

	spin_lock_irq(&css_set_lock);
	rcu_read_lock();
	cset = rcu_dereference(current->cgroups);
	list_for_each_entry(link, &cset->cgrp_links, cgrp_link) {
		struct cgroup *c = link->cgrp;

		cgroup_name(c, name_buf, NAME_MAX + 1);
		seq_printf(seq, "Root %d group %s\n",
			   c->root->hierarchy_id, name_buf);
	}
	rcu_read_unlock();
	spin_unlock_irq(&css_set_lock);
	kfree(name_buf);
	return 0;
}

#define MAX_TASKS_SHOWN_PER_CSS 25
static int cgroup_css_links_read(struct seq_file *seq, void *v)
{
	struct cgroup_subsys_state *css = seq_css(seq);
	struct cgrp_cset_link *link;
	int dead_cnt = 0, extra_refs = 0, threaded_csets = 0;

	spin_lock_irq(&css_set_lock);
	if (css->cgroup->proc_cgrp)
		seq_puts(seq, (css->cgroup->proc_cgrp == css->cgroup)
			      ? "[thread root]\n" : "[threaded]\n");

	list_for_each_entry(link, &css->cgroup->cset_links, cset_link) {
		struct css_set *cset = link->cset;
		struct task_struct *task;
		int count = 0;
		int refcnt = atomic_read(&cset->refcount);

		/*
		 * Print out the proc_cset and threaded_cset relationship
		 * and highlight difference between refcount and task_count.
		 */
		seq_printf(seq, "css_set %pK", cset);
		if (cset->proc_cset != cset) {
			threaded_csets++;
			seq_printf(seq, "=>%pK", cset->proc_cset);
		}
		if (!list_empty(&cset->threaded_csets)) {
			struct css_set *tcset;
			int idx = 0;

			list_for_each_entry(tcset, &cset->threaded_csets,
					    threaded_csets_node) {
				seq_puts(seq, idx ? "," : "<=");
				seq_printf(seq, "%pK", tcset);
				idx++;
			}
		} else {
			seq_printf(seq, " %d", refcnt);
			if (refcnt - cset->task_count > 0) {
				int extra = refcnt - cset->task_count;

				seq_printf(seq, " +%d", extra);
				/*
				 * Take out the one additional reference in
				 * init_css_set.
				 */
				if (cset == &init_css_set)
					extra--;
				extra_refs += extra;
			}
		}
		seq_puts(seq, "\n");

		list_for_each_entry(task, &cset->tasks, cg_list) {
			if (count++ <= MAX_TASKS_SHOWN_PER_CSS)
				seq_printf(seq, "  task %d\n",
					   task_pid_vnr(task));
		}

		list_for_each_entry(task, &cset->mg_tasks, cg_list) {
			if (count++ <= MAX_TASKS_SHOWN_PER_CSS)
				seq_printf(seq, "  task %d\n",
					   task_pid_vnr(task));
		}
		/* show # of overflowed tasks */
		if (count > MAX_TASKS_SHOWN_PER_CSS)
			seq_printf(seq, "  ... (%d)\n",
				   count - MAX_TASKS_SHOWN_PER_CSS);

		if (cset->dead) {
			seq_puts(seq, "    [dead]\n");
			dead_cnt++;
		}

		WARN_ON(count != cset->task_count);
	}
	spin_unlock_irq(&css_set_lock);

	if (!dead_cnt && !extra_refs && !threaded_csets)
		return 0;

	seq_puts(seq, "\n");
	if (threaded_csets)
		seq_printf(seq, "threaded css_sets = %d\n", threaded_csets);
	if (extra_refs)
		seq_printf(seq, "extra references = %d\n", extra_refs);
	if (dead_cnt)
		seq_printf(seq, "dead css_sets = %d\n", dead_cnt);

	return 0;
}

static int cgroup_subsys_states_read(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	struct cgroup_subsys *ss;
	struct cgroup_subsys_state *css;
	int i;

	mutex_lock(&cgroup_mutex);
	for_each_subsys(ss, i) {
		css = rcu_dereference_check(cgrp->subsys[ss->id], true);
		if (!css)
			continue;
		seq_printf(seq, "%2d: %-4s\t- %lx[%d] %d\n", ss->id, ss->name,
			  (unsigned long)css, css->id,
			  atomic_read(&css->online_cnt));
	}
	mutex_unlock(&cgroup_mutex);
	return 0;
}

static u64 releasable_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return (!cgroup_is_populated(css->cgroup) &&
		!css_has_online_children(&css->cgroup->self));
}

static struct cftype debug_files[] =  {
	{
		.name = "taskcount",
		.read_u64 = debug_taskcount_read,
	},

	{
		.name = "current_css_set",
		.seq_show = current_css_set_read,
		.flags = CFTYPE_ONLY_ON_ROOT,
	},

	{
		.name = "current_css_set_refcount",
		.read_u64 = current_css_set_refcount_read,
		.flags = CFTYPE_ONLY_ON_ROOT,
	},

	{
		.name = "current_css_set_cg_links",
		.seq_show = current_css_set_cg_links_read,
		.flags = CFTYPE_ONLY_ON_ROOT,
	},

	{
		.name = "cgroup_css_links",
		.seq_show = cgroup_css_links_read,
	},

	{
		.name = "cgroup_subsys_states",
		.seq_show = cgroup_subsys_states_read,
	},

	{
		.name = "releasable",
		.read_u64 = releasable_read,
	},

	{ }	/* terminate */
};

struct cgroup_subsys debug_cgrp_subsys = {
	.css_alloc	= debug_css_alloc,
	.css_free	= debug_css_free,
	.legacy_cftypes	= debug_files,
	.dfl_cftypes	= debug_files,
	.threaded	= true,
};
