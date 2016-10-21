/*
 * security/caitsith/lsm.c
 *
 * Copyright (C) 2010-2013  Tetsuo Handa <penguin-kernel@I-love.SAKURA.ne.jp>
 */

#include <linux/lsm_hooks.h>
#include "caitsith.h"

/**
 * caitsith_bprm_set_creds - Target for security_bprm_set_creds().
 *
 * @bprm: Pointer to "struct linux_binprm".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int caitsith_bprm_set_creds(struct linux_binprm *bprm)
{
	/*
	 * Do only if this function is called for the first time of an execve
	 * operation.
	 */
	if (bprm->cred_prepared)
		return 0;
#ifndef CONFIG_SECURITY_CAITSITH_OMIT_USERSPACE_LOADER
	/*
	 * Load policy if /sbin/caitsith-init exists and /sbin/init is requested
	 * for the first time.
	 */
	if (!cs_policy_loaded)
		cs_load_policy(bprm->filename);
#endif
	return cs_start_execve(bprm);
}

/*
 * caitsith_security_ops is a "struct security_operations" which is used for
 * registering CaitSith.
 */
static struct security_hook_list caitsith_hooks[] = {
	LSM_HOOK_INIT(bprm_set_creds, caitsith_bprm_set_creds),
};

/**
 * caitsith_init - Register CaitSith as a LSM module.
 *
 * Returns 0.
 */
static int __init caitsith_init(void)
{
	if (!security_module_enable("caitsith"))
		return 0;
	/* register ourselves with the security framework */
	security_add_hooks(caitsith_hooks, ARRAY_SIZE(caitsith_hooks));
	printk(KERN_INFO "CaitSith initialized\n");
	cs_init_module();
	return 0;
}

security_initcall(caitsith_init);
