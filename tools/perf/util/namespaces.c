#include "namespaces.h"
#include "util.h"
#include "event.h"
#include <stdlib.h>
#include <stdio.h>

struct namespaces *namespaces__new(struct namespaces_event *event)
{
	struct namespaces *namespaces = zalloc(sizeof(*namespaces));

	if (!namespaces)
		return NULL;

	namespaces->end_time = -1;

	if (event) {
		namespaces->dev_num = event->dev_num;
		memcpy(namespaces->inode_num, event->inode_num,
		       sizeof(namespaces->inode_num));
	}

	return namespaces;
}

void namespaces__free(struct namespaces *namespaces)
{
	free(namespaces);
}
