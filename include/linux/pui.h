#ifndef _LINUX_PUI_H
#define _LINUX_PUI_H

#ifdef CONFIG_PUI

#include <linux/atomic.h>

typedef __u64       pui_t;
typedef char        pui_str_t[17];
typedef atomic64_t  pui_gen_t;

struct pid;
struct upid;
struct task_struct;
struct pid_namespace;

#define PUI_INVALID    0
#define PUI_GEN_INIT   ATOMIC_INIT(0)

/*
 * look up a PUI in the hash table. Must be called with the tasklist_lock
 * or rcu_read_lock() held.
 *
 * find_pui_ns() finds the pui in the namespace specified
 * find_vpui() finds the pui by its virtual id, i.e. in the current namespace
 */
extern struct pid *find_pui_ns(pui_t pui, struct pid_namespace *ns);
extern struct pid *find_vpui(pui_t pui);

/*
 * find a task by its PUI
 *
 * find_task_by_pui_ns():
 *      finds a task by its pui in the specified namespace
 * find_task_by_vpui():
 *      finds a task by its virtual pui
 */
extern struct task_struct *find_task_by_pui_ns(pui_t pui, struct pid_namespace *ns);
extern struct task_struct *find_task_by_vpui(pui_t pui);

extern pui_t pui_nr_ns(struct pid *pid, struct pid_namespace *ns);
extern pui_t pui_vnr(struct pid *pid);

extern void pui_init_generator(pui_gen_t *generator);

extern void pui_make(struct upid *upid);
extern void pui_add(struct upid *upid);
extern void pui_del(struct upid *upid);

extern int pui_to_str(pui_t pui, pui_str_t str);
extern pui_t pui_from_str(const char *str);

#endif

#endif /* _LINUX_PUI_H */
