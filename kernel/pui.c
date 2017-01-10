#ifdef CONFIG_PUI

/*
#include <linux/mm.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/rculist.h>
#include <linux/bootmem.h>
#include <linux/hash.h>
#include <linux/init_task.h>
#include <linux/syscalls.h>
#include <linux/proc_ns.h>
#include <linux/proc_fs.h>
*/
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/rculist.h>
#include <linux/hash.h>
#include <linux/pui.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>

#define pui_hashfn(pui, ns)	\
	hash_long((unsigned long)pui + (unsigned long)ns, puihash_shift)

static struct hlist_head *pui_hash;
static unsigned int puihash_shift = 4;

void __init puihash_init(void)
{
	unsigned int i, pidhash_size;

	pui_hash = alloc_large_system_hash("PUI", sizeof(*pui_hash), 0, 18,
					   HASH_EARLY | HASH_SMALL,
					   &puihash_shift, NULL,
					   0, 4096);
	pidhash_size = 1U << puihash_shift;

	for (i = 0; i < pidhash_size; i++)
		INIT_HLIST_HEAD(&pui_hash[i]);
}

struct pid *find_pui_ns(pui_t pui, struct pid_namespace *ns)
{
	struct upid *pnr;

	hlist_for_each_entry_rcu(pnr,
			&pui_hash[pui_hashfn(pui, ns)], pui_chain)
		if (pnr->pui == pui && pnr->ns == ns)
			return container_of(pnr, struct pid,
					numbers[ns->level]);

	return NULL;
}

struct task_struct *find_task_by_pui_ns(pui_t pui, struct pid_namespace *ns)
{
	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			 "find_task_by_pui_ns() needs rcu_read_lock() protection");
	return pid_task(find_pui_ns(pui, ns), PIDTYPE_PID);
}

struct pid *find_vpui(pui_t pui)
{
	return find_pui_ns(pui, task_active_pid_ns(current));
}

struct task_struct *find_task_by_vpui(pui_t pui)
{
	return find_task_by_pui_ns(pui, task_active_pid_ns(current));
}

pui_t pui_nr_ns(struct pid *pid, struct pid_namespace *ns)
{
	struct upid *upid;
	pui_t result = PUI_INVALID;

	if (pid && ns->level <= pid->level) {
		upid = &pid->numbers[ns->level];
		if (upid->ns == ns)
			result = upid->pui;
	}
	return result;
}

pui_t pui_vnr(struct pid *pid)
{
	return pui_nr_ns(pid, task_active_pid_ns(current));
}

void pui_init_generator(pui_gen_t *generator)
{
	atomic64_set((atomic64_t*)generator, 0);
}

void pui_del(struct upid *upid)
{
	hlist_del_rcu(&upid->pui_chain);
}

void pui_add(struct upid *upid)
{
	hlist_add_head_rcu(&upid->pui_chain,
			&pui_hash[pui_hashfn(upid->pui, upid->ns)]);
}

static inline pui_t pui_new(pui_gen_t *generator)
{
	pui_t result;
	do { result = atomic64_inc_return((atomic64_t*)&generator); } while(result == PUI_INVALID);
	return result;
}

void pui_make(struct upid *upid)
{
	upid->pui = pui_new(&upid->ns->pui_generator);
}

int pui_to_str(pui_t pui, pui_str_t str)
{
	char c;
	int i, j, r;

	i = 0;
	do {
		c = (char)(pui & 15);
		pui >>= 4;
		str[i++] = (char)(c + (c > 9 ? 'a' - 10 : '0'));
	} while(pui);
	str[r = i] = 0;
	j = 0;
	while(j < --i) {
		c = str[i];
		str[i] = str[j];
		str[j++] = c;
	}
	return r;
}

pui_t pui_from_str(const char *str)
{
	char c;
	pui_t result = 0;

	c = *str;
	if (!c)
		return PUI_INVALID;
	do {
		if ('0' <= c && c <= '9')
			c = c - '0';
		else if ('a' <= c && c <= 'f')
			c = c - 'a' + 10;
		else if ('A' <= c && c <= 'F')
			c = c - 'A' + 10;
		else
			return PUI_INVALID;
		if (result >> 60)
			return PUI_INVALID;
		result = (result << 4) | c;
		c = *++str;
	} while(c);
	return result;
}


#endif

