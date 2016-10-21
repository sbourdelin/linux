/*
 * security/caitsith/load_policy.c
 *
 * Copyright (C) 2005-2012  NTT DATA CORPORATION
 */

#include "caitsith.h"

#ifndef CONFIG_SECURITY_CAITSITH_OMIT_USERSPACE_LOADER

/* Path to the policy loader. */
static const char *cs_loader;

/**
 * cs_loader_setup - Set policy loader.
 *
 * @str: Program to use as a policy loader (e.g. /sbin/caitsith-init ).
 *
 * Returns 0.
 */
static int __init cs_loader_setup(char *str)
{
	cs_loader = str;
	return 0;
}

__setup("CS_loader=", cs_loader_setup);

/**
 * cs_policy_loader_exists - Check whether /sbin/caitsith-init exists.
 *
 * Returns true if /sbin/caitsith-init exists, false otherwise.
 */
static bool cs_policy_loader_exists(void)
{
	struct path path;

	if (!cs_loader)
		cs_loader = CONFIG_SECURITY_CAITSITH_POLICY_LOADER;
	if (kern_path(cs_loader, LOOKUP_FOLLOW, &path) == 0) {
		path_put(&path);
		return true;
	}
	printk(KERN_INFO "Not activating CaitSith as %s does not exist.\n",
	       cs_loader);
	return false;
}

/* Path to the trigger. */
static const char *cs_trigger;

/**
 * cs_trigger_setup - Set trigger for activation.
 *
 * @str: Program to use as an activation trigger (e.g. /sbin/init ).
 *
 * Returns 0.
 */
static int __init cs_trigger_setup(char *str)
{
	cs_trigger = str;
	return 0;
}

__setup("CS_trigger=", cs_trigger_setup);

/**
 * cs_load_policy - Run external policy loader to load policy.
 *
 * @filename: The program about to start.
 *
 * Returns nothing.
 *
 * This function checks whether @filename is /sbin/init, and if so
 * invoke /sbin/caitsith-init and wait for the termination of
 * /sbin/caitsith-init and then continues invocation of /sbin/init.
 * /sbin/caitsith-init reads policy files in /etc/caitsith/ directory and
 * writes to /sys/kernel/security/caitsith/ interfaces.
 */
void cs_load_policy(const char *filename)
{
	static _Bool done;
	char *argv[2];
	char *envp[3];

	if (done)
		return;
	if (!cs_trigger)
		cs_trigger = CONFIG_SECURITY_CAITSITH_ACTIVATION_TRIGGER;
	if (strcmp(filename, cs_trigger))
		return;
	if (!cs_policy_loader_exists())
		return;
	done = 1;
	printk(KERN_INFO "Calling %s to load policy. Please wait.\n",
	       cs_loader);
	argv[0] = (char *) cs_loader;
	argv[1] = NULL;
	envp[0] = "HOME=/";
	envp[1] = "PATH=/sbin:/bin:/usr/sbin:/usr/bin";
	envp[2] = NULL;
	call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
	cs_check_profile();
}

#endif
