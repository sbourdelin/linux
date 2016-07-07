#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "symbol.h"
#include "map.h"
#include "util.h"
#include "machine.h"

int arch__fix_module_baseaddr(struct machine *machine,
		u64 *start, const char *name)
{
	char path[PATH_MAX];
	char *module_name = strdup(name);
	int len = strlen(module_name);
	FILE *file;
	int err = 0;
	u64 text_start;
	char *line = NULL;
	size_t n;
	char *sep;

	module_name[len - 1] = '\0';
	module_name += 1;
	snprintf(path, PATH_MAX, "%s/sys/module/%s/sections/.text",
				machine->root_dir, module_name);
	file = fopen(path, "r");
	if (file == NULL)
		return -1;

	len = getline(&line, &n, file);
	if (len < 0) {
		err = -1;
		goto out;
	}
	line[--len] = '\0'; /* \n */
	sep = strrchr(line, 'x');
	if (sep == NULL) {
		err = -1;
		goto out;
	}
	hex2u64(sep + 1, &text_start);

	*start = text_start;
out:
	free(line);
	fclose(file);
	free(module_name - 1);
	return err;
}
