#include "perf.h"
#include "annotate.h"

struct ins *ins__find(const char *name)
{
	int i;
	struct ins *ins;

	ins = zalloc(sizeof(struct ins));
	if (!ins)
		return NULL;

	ins->name = strdup(name);
	if (!ins->name)
		return NULL;

	if (name[0] == 'b') {
		/* branch instructions */
		ins->ops = &jump_ops;

		/* these start with 'b', but aren't branch instructions */
		if (!strncmp(name, "bcd", 3) ||
				!strncmp(name, "brinc", 5) ||
				!strncmp(name, "bper", 4))
			return NULL;

		i = strlen(name) - 1;
		if (i < 0)
			return NULL;

		/* ignore optional hints at the end of the instructions */
		if (name[i] == '+' || name[i] == '-')
			i--;

		if (name[i] == 'l' || (name[i] == 'a' && name[i-1] == 'l')) {
			/*
			 * if the instruction ends up with 'l' or 'la', then
			 * those are considered 'calls' since they update LR.
			 * ... except for 'bnl' which is branch if not less than
			 * and the absolute form of the same.
			 */
			if (strcmp(name, "bnl") && strcmp(name, "bnl+") &&
			    strcmp(name, "bnl-") && strcmp(name, "bnla") &&
			    strcmp(name, "bnla+") && strcmp(name, "bnla-"))
				ins->ops = &call_ops;
		}
		if (name[i] == 'r' && name[i-1] == 'l')
			/*
			 * instructions ending with 'lr' are considered to be
			 * return instructions
			 */
			ins->ops = &ret_ops;

		return ins;
	}

	return NULL;
}
