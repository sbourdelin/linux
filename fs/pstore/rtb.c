// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 The Linux Foundation. All rights reserved.
 */

#include <linux/fs.h>
#include <linux/irqflags.h>
#include <linux/rtb.h>
#include <linux/smp.h>

#include "internal.h"

void notrace pstore_rtb_call(struct rtb_layout *start)
{
	unsigned long flags;
	struct pstore_record record = {
		.type = PSTORE_TYPE_RTB,
		.buf = (char *)start,
		.size = sizeof(*start),
		.psi = psinfo,
	};

	local_irq_save(flags);

	psinfo->write(&record);

	local_irq_restore(flags);
}

void pstore_register_rtb(void)
{
	int ret;

	if (!psinfo->write)
		return;

	ret = rtb_init();
	if (ret)
		return;
}

void pstore_unregister_rtb(void)
{
	rtb_exit();
}
