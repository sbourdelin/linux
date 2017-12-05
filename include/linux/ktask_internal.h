/*
 * ktask_internal.h
 *
 * Framework to parallelize CPU-intensive kernel work such as zeroing
 * huge pages or freeing many pages at once.  For more information, see
 * Documentation/core-api/ktask.rst.
 *
 * This file contains implementation details of ktask for core kernel code that
 * needs to be aware of them.  ktask clients should not include this file.
 */
#ifndef _LINUX_KTASK_INTERNAL_H
#define _LINUX_KTASK_INTERNAL_H

#include <linux/ktask.h>

#ifdef CONFIG_KTASK
/* Caps the number of threads that are allowed to be used in one task. */
extern int ktask_max_threads;

#endif /* CONFIG_KTASK */

#endif /* _LINUX_KTASK_INTERNAL_H */
