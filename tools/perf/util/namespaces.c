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
		memcpy(namespaces->link_info, event->link_info,
		       sizeof(namespaces->link_info));
	}

	return namespaces;
}

void namespaces__free(struct namespaces *namespaces)
{
	free(namespaces);
}
