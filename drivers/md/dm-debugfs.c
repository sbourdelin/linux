#include "dm-core.h"
#include "dm-debugfs.h"

/* See also the DMF_* defines in dm.c */
static const char *const md_flag_name[] = {
	[0] = "BLOCK_IO_FOR_SUSPEND",
	[1] = "SUSPENDED",
	[2] = "FROZEN",
	[3] = "FREEING",
	[4] = "DELETING",
	[5] = "NOFLUSH_SUSPENDING",
	[6] = "DEFERRED_REMOVE",
	[7] = "SUSPENDED_INTERNALLY",
};

void dm_mq_show_q(struct seq_file *m, struct request_queue *q)
{
	struct mapped_device *md = q->queuedata;
	struct target_type *tt = dm_get_immutable_target_type(md);
	unsigned int i;

	for (i = 0; i < sizeof(md->flags) * BITS_PER_BYTE; i++) {
		if (!(md->flags & BIT(i)))
			continue;
		if (i < ARRAY_SIZE(md_flag_name) && md_flag_name[i])
			seq_printf(m, " %s", md_flag_name[i]);
		else
			seq_printf(m, " %d", i);
	}
	if (tt->show)
		tt->show(m, md->immutable_target);
}

