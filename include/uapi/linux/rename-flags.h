/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_RENAME_H
#define _UAPI_LINUX_RENAME_H

/*
 * Definitions for rename syscall family.
 */
#define RENAME_NOREPLACE	(1 << 0)	/* Don't overwrite target */
#define RENAME_EXCHANGE		(1 << 1)	/* Exchange source and dest */
#define RENAME_WHITEOUT		(1 << 2)	/* Whiteout source */

#endif /* _UAPI_LINUX_RENAME_H */
