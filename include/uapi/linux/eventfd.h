/*
 *  Copyright (C) 2013 Martin Sustrik <sustrik@250bpm.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef _UAPI_LINUX_EVENTFD_H
#define _UAPI_LINUX_EVENTFD_H

/* For O_CLOEXEC */
#include <linux/fcntl.h>
#include <linux/types.h>

/*
 * CAREFUL: Check include/asm-generic/fcntl.h when defining
 * new flags, since they might collide with O_* ones. We want
 * to re-use O_* flags that couldn't possibly have a meaning
 * from eventfd, in order to leave a free define-space for
 * shared O_* flags.
 */

/* Provide semaphore-like semantics for reads from the eventfd. */
#define EFD_SEMAPHORE (1 << 0)
/* Provide event mask semantics for the eventfd. */
#define EFD_MASK (1 << 1)
/*  Set the close-on-exec (FD_CLOEXEC) flag on the eventfd. */
#define EFD_CLOEXEC O_CLOEXEC
/*  Create the eventfd in non-blocking mode. */
#define EFD_NONBLOCK O_NONBLOCK
#endif /* _UAPI_LINUX_EVENTFD_H */
