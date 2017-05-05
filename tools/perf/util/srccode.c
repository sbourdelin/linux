/* Manage printing source lines */
#include "linux/list.h"
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include "srccode.h"
#include "debug.h"

#define MAXSRCCACHE (32*1024*1024)

struct srcfile {
	struct list_head nd;
	char *fn;
	char **lines;
	char *map;
	unsigned numlines;
	size_t maplen;
};

/*
 * Could use a hash table, but for now assume move-to-front is good enough
 * with spatial locality.
 */
static LIST_HEAD(srcfile_list);
static long map_total_sz;

static int countlines(char *map, int maplen)
{
	int numl;
	char *end = map + maplen;
	char *p = map;

	if (maplen == 0)
		return 0;
	numl = 0;
	while (p < end && (p = memchr(p, '\n', end - p)) != NULL) {
		numl++;
		p++;
	}
	if (p < end)
		numl++;
	return numl;
}

static void fill_lines(char **lines, int maxline, char *map, int maplen)
{
	int l;
	char *end = map + maplen;
	char *p = map;

	if (maplen == 0 || maxline == 0)
		return;
	l = 0;
	lines[l++] = map;
	while (p < end && (p = memchr(p, '\n', end - p)) != NULL) {
		if (l >= maxline)
			return;
		lines[l++] = ++p;
	}
	if (p < end)
		lines[l] = p;
}

static void free_srcfile(struct srcfile *sf)
{
	list_del(&sf->nd);
	map_total_sz -= sf->maplen;
	munmap(sf->map, sf->maplen);
	free(sf->lines);
	free(sf->fn);
	free(sf);
}

static struct srcfile *find_srcfile(char *fn)
{
	struct stat st;
	struct srcfile *h;
	int fd;
	unsigned long ps, sz;
	bool first = true;

	list_for_each_entry (h, &srcfile_list, nd) {
		if (!strcmp(fn, h->fn)) {
			if (!first) {
				/* Move to front */
				list_del(&h->nd);
				list_add(&h->nd, &srcfile_list);
			}
			return h;
		}
		first = false;
	}

	/* Only prune if there is more than one entry */
	while (map_total_sz > MAXSRCCACHE &&
	       srcfile_list.next != &srcfile_list) {
		assert(!list_empty(&srcfile_list));
		h = list_entry(srcfile_list.prev, struct srcfile, nd);
		free_srcfile(h);
	}

	fd = open(fn, O_RDONLY);
	if (fd < 0 || fstat(fd, &st) < 0) {
		pr_debug("cannot open source file %s\n", fn);
		return NULL;
	}

	h = malloc(sizeof(struct srcfile));
	if (!h)
		return NULL;

	h->fn = strdup(fn);
	if (!h->fn)
		goto out_h;

	h->maplen = st.st_size;
	ps = sysconf(_SC_PAGE_SIZE);
	sz = (h->maplen + ps - 1) & ~(ps - 1);
	h->map = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (h->map == (char *)-1) {
		pr_debug("cannot mmap source file %s\n", fn);
		goto out_fn;
	}
	h->numlines = countlines(h->map, h->maplen);
	h->lines = calloc(h->numlines, sizeof(char *));
	if (!h->lines)
		goto out_map;
	fill_lines(h->lines, h->numlines, h->map, h->maplen);
	list_add(&h->nd, &srcfile_list);
	map_total_sz += h->maplen;
	return h;

out_map:
	munmap(h->map, sz);
out_fn:
	free(h->fn);
out_h:
	free(h);
	return NULL;
}

/* Result is not 0 terminated */
char *find_sourceline(char *fn, unsigned line, int *lenp)
{
	char *l, *p;
	struct srcfile *sf = find_srcfile(fn);
	if (!sf)
		return NULL;
	line--;
	if (line >= sf->numlines)
		return NULL;
	l = sf->lines[line];
	if (!l)
		return NULL;
	p = memchr(l, '\n', sf->map + sf->maplen - l);
	*lenp = p - l;
	return l;
}
