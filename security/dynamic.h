#include <linux/lsm_hooks.h>
#include <linux/srcu.h>
#include <linux/list.h>
#include <linux/jump_label.h>
#include <linux/percpu_counter.h>
#include <linux/percpu.h>
#include <linux/fs.h>

#ifdef CONFIG_SECURITY_DYNAMIC_HOOKS
extern struct static_key_false dynamic_hooks_keys[];

struct dynamic_hook {
	struct percpu_counter	invocation;
	struct percpu_counter	deny;
	const char		*name;
	struct list_head	head;
	struct srcu_struct	srcu;
};

extern struct dynamic_hook dynamic_hooks[];
extern void security_init_dynamic_hooks(void);
#else
static void security_init_dynamic_hooks(void) {}
#endif
