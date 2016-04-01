/*
 * NSALinux services exported to the rest of the kernel.
 *
 * Author: James Morris <jmorris@redhat.com>
 *
 * Copyright (C) 2005 Red Hat, Inc., James Morris <jmorris@redhat.com>
 * Copyright (C) 2006 Trusted Computer Solutions, Inc. <dgoeddel@trustedcs.com>
 * Copyright (C) 2006 IBM Corporation, Timothy R. Chavez <tinytim@us.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2,
 * as published by the Free Software Foundation.
 */
#ifndef _LINUX_NSALINUX_H
#define _LINUX_NSALINUX_H

struct nsalinux_audit_rule;
struct audit_context;
struct kern_ipc_perm;

#ifdef CONFIG_SECURITY_NSALINUX

/**
 * nsalinux_is_enabled - is NSALinux enabled?
 */
bool nsalinux_is_enabled(void);
#else

static inline bool nsalinux_is_enabled(void)
{
	return false;
}
#endif	/* CONFIG_SECURITY_NSALINUX */

#endif /* _LINUX_NSALINUX_H */
