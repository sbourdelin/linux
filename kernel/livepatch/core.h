#ifndef _LIVEPATCH_CORE_H
#define _LIVEPATCH_CORE_H

#include <linux/livepatch.h>

extern struct mutex klp_mutex;

void klp_patch_free_no_ops(struct klp_patch *patch);

#endif /* _LIVEPATCH_CORE_H */
