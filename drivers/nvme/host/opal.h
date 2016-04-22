/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Rafael Antognolli <rafael.antognolli@intel.com>
 */

#ifndef _NVME_OPAL_H
#define _NVME_OPAL_H

#include <linux/nvme_ioctl.h>
/* TODO: Put this inside ifdef.
 * Need to make function parameters more opaque.
 */
#include "nvme.h"

#ifdef CONFIG_BLK_DEV_NVME_OPAL

int nvme_opal_init(void);
void nvme_opal_exit(void);

int nvme_opal_register(struct nvme_ns *ns, struct nvme_opal_key __user *arg);
void nvme_opal_unregister(struct nvme_ns *ns, uint8_t locking_range);

int nvme_opal_unlock(struct nvme_ns *ns);

#else

static inline int nvme_opal_init(void)
{
	return 0;
}

static inline void nvme_opal_exit(void)
{
}

static inline int nvme_opal_register(struct nvme_ns *ns, struct nvme_opal_key __user *arg)
{
	return -ENOTTY;
}

static inline void nvme_opal_unregister(struct nvme_ns *ns, uint8_t locking_range)
{
}

static inline int nvme_opal_unlock(struct nvme_ns *ns)
{
	return -ENOTTY;
}

#endif

#endif /* _NVME_OPAL_H */
