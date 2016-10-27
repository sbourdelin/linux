/*
 * Scheduler Tunability (SchedTune) Extensions for CFS
 *
 * Copyright (C) 2016 ARM Ltd, Patrick Bellasi <patrick.bellasi@arm.com>
 */

#ifdef CONFIG_SCHED_TUNE

#include <linux/reciprocal_div.h>

extern struct reciprocal_value schedtune_spc_rdiv;

#endif /* CONFIG_SCHED_TUNE */
