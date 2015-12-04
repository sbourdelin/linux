#include "compat-util.h"
#include "cfg.h"

#define UNDEFINED "UNDEFINED"

struct util_cfg util_cfg = {
	.prefix		= UNDEFINED,
	.exec_name	= UNDEFINED,
	.exec_path	= UNDEFINED,
	.exec_path_env	= UNDEFINED,
	.pager_env	= UNDEFINED,
};
