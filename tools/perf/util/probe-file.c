/*
 * probe-file.c : operate ftrace k/uprobe events files
 *
 * Written by Masami Hiramatsu <masami.hiramatsu.pt@hitachi.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <sys/uio.h>
#include "util.h"
#include "event.h"
#include "strlist.h"
#include "debug.h"
#include "cache.h"
#include "color.h"
#include "symbol.h"
#include "thread.h"
#include <api/fs/tracing_path.h>
#include "probe-event.h"
#include "probe-file.h"
#include "session.h"
#include "probe-finder.h"

#define MAX_CMDLEN 256
#define MAX_EVENT_LENGTH 512

static void print_open_warning(int err, bool uprobe)
{
	char sbuf[STRERR_BUFSIZE];

	if (err == -ENOENT) {
		const char *config;

		if (uprobe)
			config = "CONFIG_UPROBE_EVENTS";
		else
			config = "CONFIG_KPROBE_EVENTS";

		pr_warning("%cprobe_events file does not exist"
			   " - please rebuild kernel with %s.\n",
			   uprobe ? 'u' : 'k', config);
	} else if (err == -ENOTSUP)
		pr_warning("Tracefs or debugfs is not mounted.\n");
	else
		pr_warning("Failed to open %cprobe_events: %s\n",
			   uprobe ? 'u' : 'k',
			   str_error_r(-err, sbuf, sizeof(sbuf)));
}

static void print_both_open_warning(int kerr, int uerr)
{
	/* Both kprobes and uprobes are disabled, warn it. */
	if (kerr == -ENOTSUP && uerr == -ENOTSUP)
		pr_warning("Tracefs or debugfs is not mounted.\n");
	else if (kerr == -ENOENT && uerr == -ENOENT)
		pr_warning("Please rebuild kernel with CONFIG_KPROBE_EVENTS "
			   "or/and CONFIG_UPROBE_EVENTS.\n");
	else {
		char sbuf[STRERR_BUFSIZE];
		pr_warning("Failed to open kprobe events: %s.\n",
			   str_error_r(-kerr, sbuf, sizeof(sbuf)));
		pr_warning("Failed to open uprobe events: %s.\n",
			   str_error_r(-uerr, sbuf, sizeof(sbuf)));
	}
}

static int open_probe_events(const char *trace_file, bool readwrite)
{
	char buf[PATH_MAX];
	int ret;

	ret = e_snprintf(buf, PATH_MAX, "%s/%s",
			 tracing_path, trace_file);
	if (ret >= 0) {
		pr_debug("Opening %s write=%d\n", buf, readwrite);
		if (readwrite && !probe_event_dry_run)
			ret = open(buf, O_RDWR | O_APPEND, 0);
		else
			ret = open(buf, O_RDONLY, 0);

		if (ret < 0)
			ret = -errno;
	}
	return ret;
}

static int open_kprobe_events(bool readwrite)
{
	return open_probe_events("kprobe_events", readwrite);
}

static int open_uprobe_events(bool readwrite)
{
	return open_probe_events("uprobe_events", readwrite);
}

int probe_file__open(int flag)
{
	int fd;

	if (flag & PF_FL_UPROBE)
		fd = open_uprobe_events(flag & PF_FL_RW);
	else
		fd = open_kprobe_events(flag & PF_FL_RW);
	if (fd < 0)
		print_open_warning(fd, flag & PF_FL_UPROBE);

	return fd;
}

int probe_file__open_both(int *kfd, int *ufd, int flag)
{
	if (!kfd || !ufd)
		return -EINVAL;

	*kfd = open_kprobe_events(flag & PF_FL_RW);
	*ufd = open_uprobe_events(flag & PF_FL_RW);
	if (*kfd < 0 && *ufd < 0) {
		print_both_open_warning(*kfd, *ufd);
		return *kfd;
	}

	return 0;
}

/* Get raw string list of current kprobe_events  or uprobe_events */
struct strlist *probe_file__get_rawlist(int fd)
{
	int ret, idx, fddup;
	FILE *fp;
	char buf[MAX_CMDLEN];
	char *p;
	struct strlist *sl;

	if (fd < 0)
		return NULL;

	sl = strlist__new(NULL, NULL);
	if (sl == NULL)
		return NULL;

	fddup = dup(fd);
	if (fddup < 0)
		goto out_free_sl;

	fp = fdopen(fddup, "r");
	if (!fp)
		goto out_close_fddup;

	while (!feof(fp)) {
		p = fgets(buf, MAX_CMDLEN, fp);
		if (!p)
			break;

		idx = strlen(p) - 1;
		if (p[idx] == '\n')
			p[idx] = '\0';
		ret = strlist__add(sl, buf);
		if (ret < 0) {
			pr_debug("strlist__add failed (%d)\n", ret);
			goto out_close_fp;
		}
	}
	fclose(fp);

	return sl;

out_close_fp:
	fclose(fp);
	goto out_free_sl;
out_close_fddup:
	close(fddup);
out_free_sl:
	strlist__delete(sl);
	return NULL;
}

static struct strlist *__probe_file__get_namelist(int fd, bool include_group)
{
	char buf[128];
	struct strlist *sl, *rawlist;
	struct str_node *ent;
	struct probe_trace_event tev;
	int ret = 0;

	memset(&tev, 0, sizeof(tev));
	rawlist = probe_file__get_rawlist(fd);
	if (!rawlist)
		return NULL;
	sl = strlist__new(NULL, NULL);
	strlist__for_each_entry(ent, rawlist) {
		ret = parse_probe_trace_command(ent->s, &tev);
		if (ret < 0)
			break;
		if (include_group) {
			ret = e_snprintf(buf, 128, "%s:%s", tev.group,
					tev.event);
			if (ret >= 0)
				ret = strlist__add(sl, buf);
		} else
			ret = strlist__add(sl, tev.event);
		clear_probe_trace_event(&tev);
		if (ret < 0)
			break;
	}
	strlist__delete(rawlist);

	if (ret < 0) {
		strlist__delete(sl);
		return NULL;
	}
	return sl;
}

/* Get current perf-probe event names */
struct strlist *probe_file__get_namelist(int fd)
{
	return __probe_file__get_namelist(fd, false);
}

int probe_file__add_event(int fd, struct probe_trace_event *tev)
{
	int ret = 0;
	char *buf = synthesize_probe_trace_command(tev);
	char sbuf[STRERR_BUFSIZE];

	if (!buf) {
		pr_debug("Failed to synthesize probe trace event.\n");
		return -EINVAL;
	}

	pr_debug("Writing event: %s\n", buf);
	if (!probe_event_dry_run) {
		if (write(fd, buf, strlen(buf)) < (int)strlen(buf)) {
			ret = -errno;
			pr_warning("Failed to write event: %s\n",
				   str_error_r(errno, sbuf, sizeof(sbuf)));
		}
	}
	free(buf);

	return ret;
}

static int __del_trace_probe_event(int fd, struct str_node *ent)
{
	char *p;
	char buf[128];
	int ret;

	/* Convert from perf-probe event to trace-probe event */
	ret = e_snprintf(buf, 128, "-:%s", ent->s);
	if (ret < 0)
		goto error;

	p = strchr(buf + 2, ':');
	if (!p) {
		pr_debug("Internal error: %s should have ':' but not.\n",
			 ent->s);
		ret = -ENOTSUP;
		goto error;
	}
	*p = '/';

	pr_debug("Writing event: %s\n", buf);
	ret = write(fd, buf, strlen(buf));
	if (ret < 0) {
		ret = -errno;
		goto error;
	}

	return 0;
error:
	pr_warning("Failed to delete event: %s\n",
		   str_error_r(-ret, buf, sizeof(buf)));
	return ret;
}

int probe_file__get_events(int fd, struct strfilter *filter,
			   struct strlist *plist)
{
	struct strlist *namelist;
	struct str_node *ent;
	const char *p;
	int ret = -ENOENT;

	if (!plist)
		return -EINVAL;

	namelist = __probe_file__get_namelist(fd, true);
	if (!namelist)
		return -ENOENT;

	strlist__for_each_entry(ent, namelist) {
		p = strchr(ent->s, ':');
		if ((p && strfilter__compare(filter, p + 1)) ||
		    strfilter__compare(filter, ent->s)) {
			strlist__add(plist, ent->s);
			ret = 0;
		}
	}
	strlist__delete(namelist);

	return ret;
}

int probe_file__del_strlist(int fd, struct strlist *namelist)
{
	int ret = 0;
	struct str_node *ent;

	strlist__for_each_entry(ent, namelist) {
		ret = __del_trace_probe_event(fd, ent);
		if (ret < 0)
			break;
	}
	return ret;
}

int probe_file__del_events(int fd, struct strfilter *filter)
{
	struct strlist *namelist;
	int ret;

	namelist = strlist__new(NULL, NULL);
	if (!namelist)
		return -ENOMEM;

	ret = probe_file__get_events(fd, filter, namelist);
	if (ret < 0)
		return ret;

	ret = probe_file__del_strlist(fd, namelist);
	strlist__delete(namelist);

	return ret;
}

/* Caller must ensure to remove this entry from list */
static void probe_cache_entry__delete(struct probe_cache_entry *entry)
{
	if (entry) {
		BUG_ON(!list_empty(&entry->node));

		strlist__delete(entry->tevlist);
		clear_perf_probe_event(&entry->pev);
		zfree(&entry->spev);
		free(entry);
	}
}

static struct probe_cache_entry *
probe_cache_entry__new(struct perf_probe_event *pev)
{
	struct probe_cache_entry *entry = zalloc(sizeof(*entry));

	if (entry) {
		INIT_LIST_HEAD(&entry->node);
		entry->tevlist = strlist__new(NULL, NULL);
		if (!entry->tevlist)
			zfree(&entry);
		else if (pev) {
			entry->spev = synthesize_perf_probe_command(pev);
			if (!entry->spev ||
			    perf_probe_event__copy(&entry->pev, pev) < 0) {
				probe_cache_entry__delete(entry);
				return NULL;
			}
		}
	}

	return entry;
}

int probe_cache_entry__get_event(struct probe_cache_entry *entry,
				 struct probe_trace_event **tevs)
{
	struct probe_trace_event *tev;
	struct str_node *node;
	int ret, i;

	ret = strlist__nr_entries(entry->tevlist);
	if (ret > probe_conf.max_probes)
		return -E2BIG;

	*tevs = zalloc(ret * sizeof(*tev));
	if (!*tevs)
		return -ENOMEM;

	i = 0;
	strlist__for_each_entry(node, entry->tevlist) {
		tev = &(*tevs)[i++];
		ret = parse_probe_trace_command(node->s, tev);
		if (ret < 0)
			break;
	}
	return i;
}

/* For the kernel probe caches, pass target = NULL or DSO__NAME_KALLSYMS */
static int probe_cache__open(struct probe_cache *pcache, const char *target)
{
	char cpath[PATH_MAX];
	char sbuildid[SBUILD_ID_SIZE];
	char *dir_name = NULL;
	bool is_kallsyms = false;
	int ret, fd;

	if (target && build_id_cache__cached(target)) {
		/* This is a cached buildid */
		strncpy(sbuildid, target, SBUILD_ID_SIZE);
		dir_name = build_id_cache__linkname(sbuildid, NULL, 0);
		goto found;
	}

	if (!target || !strcmp(target, DSO__NAME_KALLSYMS)) {
		target = DSO__NAME_KALLSYMS;
		is_kallsyms = true;
		ret = sysfs__sprintf_build_id("/", sbuildid);
	} else
		ret = filename__sprintf_build_id(target, sbuildid);

	if (ret < 0) {
		pr_debug("Failed to get build-id from %s.\n", target);
		return ret;
	}

	/* If we have no buildid cache, make it */
	if (!build_id_cache__cached(sbuildid)) {
		ret = build_id_cache__add_s(sbuildid, target,
					    is_kallsyms, NULL);
		if (ret < 0) {
			pr_debug("Failed to add build-id cache: %s\n", target);
			return ret;
		}
	}

	dir_name = build_id_cache__cachedir(sbuildid, target, is_kallsyms,
					    false);
found:
	if (!dir_name) {
		pr_debug("Failed to get cache from %s\n", target);
		return -ENOMEM;
	}

	snprintf(cpath, PATH_MAX, "%s/probes", dir_name);
	fd = open(cpath, O_CREAT | O_RDWR, 0644);
	if (fd < 0)
		pr_debug("Failed to open cache(%d): %s\n", fd, cpath);
	free(dir_name);
	pcache->fd = fd;

	return fd;
}

static int probe_cache__load(struct probe_cache *pcache)
{
	struct probe_cache_entry *entry = NULL;
	char buf[MAX_CMDLEN], *p;
	int ret = 0, fddup;
	FILE *fp;

	fddup = dup(pcache->fd);
	if (fddup < 0)
		return -errno;
	fp = fdopen(fddup, "r");
	if (!fp) {
		close(fddup);
		return -EINVAL;
	}

	while (!feof(fp)) {
		if (!fgets(buf, MAX_CMDLEN, fp))
			break;
		p = strchr(buf, '\n');
		if (p)
			*p = '\0';
		/* #perf_probe_event or %sdt_event */
		if (buf[0] == '#' || buf[0] == '%') {
			entry = probe_cache_entry__new(NULL);
			if (!entry) {
				ret = -ENOMEM;
				goto out;
			}
			if (buf[0] == '%')
				entry->sdt = true;
			entry->spev = strdup(buf + 1);
			if (entry->spev)
				ret = parse_perf_probe_command(buf + 1,
								&entry->pev);
			else
				ret = -ENOMEM;
			if (ret < 0) {
				probe_cache_entry__delete(entry);
				goto out;
			}
			list_add_tail(&entry->node, &pcache->entries);
		} else {	/* trace_probe_event */
			if (!entry) {
				ret = -EINVAL;
				goto out;
			}
			strlist__add(entry->tevlist, buf);
		}
	}
out:
	fclose(fp);
	return ret;
}

static struct probe_cache *probe_cache__alloc(void)
{
	struct probe_cache *pcache = zalloc(sizeof(*pcache));

	if (pcache) {
		INIT_LIST_HEAD(&pcache->entries);
		pcache->fd = -EINVAL;
	}
	return pcache;
}

void probe_cache__purge(struct probe_cache *pcache)
{
	struct probe_cache_entry *entry, *n;

	list_for_each_entry_safe(entry, n, &pcache->entries, node) {
		list_del_init(&entry->node);
		probe_cache_entry__delete(entry);
	}
}

void probe_cache__delete(struct probe_cache *pcache)
{
	if (!pcache)
		return;

	probe_cache__purge(pcache);
	if (pcache->fd > 0)
		close(pcache->fd);
	free(pcache);
}

struct probe_cache *probe_cache__new(const char *target)
{
	struct probe_cache *pcache = probe_cache__alloc();
	int ret;

	if (!pcache)
		return NULL;

	ret = probe_cache__open(pcache, target);
	if (ret < 0) {
		pr_debug("Cache open error: %d\n", ret);
		goto out_err;
	}

	ret = probe_cache__load(pcache);
	if (ret < 0) {
		pr_debug("Cache read error: %d\n", ret);
		goto out_err;
	}

	return pcache;

out_err:
	probe_cache__delete(pcache);
	return NULL;
}

static bool streql(const char *a, const char *b)
{
	if (a == b)
		return true;

	if (!a || !b)
		return false;

	return !strcmp(a, b);
}

struct probe_cache_entry *
probe_cache__find(struct probe_cache *pcache, struct perf_probe_event *pev)
{
	struct probe_cache_entry *entry = NULL;
	char *cmd = synthesize_perf_probe_command(pev);

	if (!cmd)
		return NULL;

	for_each_probe_cache_entry(entry, pcache) {
		if (pev->sdt) {
			if (entry->pev.event &&
			    streql(entry->pev.event, pev->event) &&
			    (!pev->group ||
			     streql(entry->pev.group, pev->group)))
				goto found;

			continue;
		}
		/* Hit if same event name or same command-string */
		if ((pev->event &&
		     (streql(entry->pev.group, pev->group) &&
		      streql(entry->pev.event, pev->event))) ||
		    (!strcmp(entry->spev, cmd)))
			goto found;
	}
	entry = NULL;

found:
	free(cmd);
	return entry;
}

struct probe_cache_entry *
probe_cache__find_by_name(struct probe_cache *pcache,
			  const char *group, const char *event)
{
	struct probe_cache_entry *entry = NULL;

	for_each_probe_cache_entry(entry, pcache) {
		/* Hit if same event name or same command-string */
		if (streql(entry->pev.group, group) &&
		    streql(entry->pev.event, event))
			goto found;
	}
	entry = NULL;

found:
	return entry;
}

int probe_cache__add_entry(struct probe_cache *pcache,
			   struct perf_probe_event *pev,
			   struct probe_trace_event *tevs, int ntevs)
{
	struct probe_cache_entry *entry = NULL;
	char *command;
	int i, ret = 0;

	if (!pcache || !pev || !tevs || ntevs <= 0) {
		ret = -EINVAL;
		goto out_err;
	}

	/* Remove old cache entry */
	entry = probe_cache__find(pcache, pev);
	if (entry) {
		list_del_init(&entry->node);
		probe_cache_entry__delete(entry);
	}

	ret = -ENOMEM;
	entry = probe_cache_entry__new(pev);
	if (!entry)
		goto out_err;

	for (i = 0; i < ntevs; i++) {
		if (!tevs[i].point.symbol)
			continue;

		command = synthesize_probe_trace_command(&tevs[i]);
		if (!command)
			goto out_err;
		strlist__add(entry->tevlist, command);
		free(command);
	}
	list_add_tail(&entry->node, &pcache->entries);
	pr_debug("Added probe cache: %d\n", ntevs);
	return 0;

out_err:
	pr_debug("Failed to add probe caches\n");
	probe_cache_entry__delete(entry);
	return ret;
}

#ifdef HAVE_GELF_GETNOTE_SUPPORT
static unsigned long long sdt_note__get_addr(struct sdt_note *note)
{
	return note->bit32 ? (unsigned long long)note->addr.a32[0]
		 : (unsigned long long)note->addr.a64[0];
}

int probe_cache__scan_sdt(struct probe_cache *pcache, const char *pathname)
{
	struct probe_cache_entry *entry = NULL;
	struct list_head sdtlist;
	struct sdt_note *note;
	char *buf;
	char sdtgrp[64];
	int ret;

	INIT_LIST_HEAD(&sdtlist);
	ret = get_sdt_note_list(&sdtlist, pathname);
	if (ret < 0) {
		pr_debug4("Failed to get sdt note: %d\n", ret);
		return ret;
	}
	list_for_each_entry(note, &sdtlist, note_list) {
		ret = snprintf(sdtgrp, 64, "sdt_%s", note->provider);
		if (ret < 0)
			break;
		/* Try to find same-name entry */
		entry = probe_cache__find_by_name(pcache, sdtgrp, note->name);
		if (!entry) {
			entry = probe_cache_entry__new(NULL);
			if (!entry) {
				ret = -ENOMEM;
				break;
			}
			entry->sdt = true;
			ret = asprintf(&entry->spev, "%s:%s=%s", sdtgrp,
					note->name, note->name);
			if (ret < 0)
				break;
			entry->pev.event = strdup(note->name);
			entry->pev.group = strdup(sdtgrp);
			list_add_tail(&entry->node, &pcache->entries);
		}
		ret = asprintf(&buf, "p:%s/%s %s:0x%llx",
				sdtgrp, note->name, pathname,
				sdt_note__get_addr(note));
		if (ret < 0)
			break;
		strlist__add(entry->tevlist, buf);
		free(buf);
		entry = NULL;
	}
	if (entry) {
		list_del_init(&entry->node);
		probe_cache_entry__delete(entry);
	}
	cleanup_sdt_note_list(&sdtlist);
	return ret;
}
#endif

static int probe_cache_entry__write(struct probe_cache_entry *entry, int fd)
{
	struct str_node *snode;
	struct stat st;
	struct iovec iov[3];
	const char *prefix = entry->sdt ? "%" : "#";
	int ret;
	/* Save stat for rollback */
	ret = fstat(fd, &st);
	if (ret < 0)
		return ret;

	pr_debug("Writing cache: %s%s\n", prefix, entry->spev);
	iov[0].iov_base = (void *)prefix; iov[0].iov_len = 1;
	iov[1].iov_base = entry->spev; iov[1].iov_len = strlen(entry->spev);
	iov[2].iov_base = (void *)"\n"; iov[2].iov_len = 1;
	ret = writev(fd, iov, 3);
	if (ret < (int)iov[1].iov_len + 2)
		goto rollback;

	strlist__for_each_entry(snode, entry->tevlist) {
		iov[0].iov_base = (void *)snode->s;
		iov[0].iov_len = strlen(snode->s);
		iov[1].iov_base = (void *)"\n"; iov[1].iov_len = 1;
		ret = writev(fd, iov, 2);
		if (ret < (int)iov[0].iov_len + 1)
			goto rollback;
	}
	return 0;

rollback:
	/* Rollback to avoid cache file corruption */
	if (ret > 0)
		ret = -1;
	if (ftruncate(fd, st.st_size) < 0)
		ret = -2;

	return ret;
}

int probe_cache__commit(struct probe_cache *pcache)
{
	struct probe_cache_entry *entry;
	int ret = 0;

	/* TBD: if we do not update existing entries, skip it */
	ret = lseek(pcache->fd, 0, SEEK_SET);
	if (ret < 0)
		goto out;

	ret = ftruncate(pcache->fd, 0);
	if (ret < 0)
		goto out;

	for_each_probe_cache_entry(entry, pcache) {
		ret = probe_cache_entry__write(entry, pcache->fd);
		pr_debug("Cache committed: %d\n", ret);
		if (ret < 0)
			break;
	}
out:
	return ret;
}

static bool probe_cache_entry__compare(struct probe_cache_entry *entry,
				       struct strfilter *filter)
{
	char buf[128], *ptr = entry->spev;

	if (entry->pev.event) {
		snprintf(buf, 128, "%s:%s", entry->pev.group, entry->pev.event);
		ptr = buf;
	}
	return strfilter__compare(filter, ptr);
}

int probe_cache__filter_purge(struct probe_cache *pcache,
			      struct strfilter *filter)
{
	struct probe_cache_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &pcache->entries, node) {
		if (probe_cache_entry__compare(entry, filter)) {
			pr_info("Removed cached event: %s\n", entry->spev);
			list_del_init(&entry->node);
			probe_cache_entry__delete(entry);
		}
	}
	return 0;
}

static int probe_cache__show_entries(struct probe_cache *pcache,
				     struct strfilter *filter)
{
	struct probe_cache_entry *entry;

	for_each_probe_cache_entry(entry, pcache) {
		if (probe_cache_entry__compare(entry, filter))
			printf("%s\n", entry->spev);
	}
	return 0;
}

/* Show all cached probes */
int probe_cache__show_all_caches(struct strfilter *filter)
{
	struct probe_cache *pcache;
	struct strlist *bidlist;
	struct str_node *nd;
	char *buf = strfilter__string(filter);

	pr_debug("list cache with filter: %s\n", buf);
	free(buf);

	bidlist = build_id_cache__list_all(true);
	if (!bidlist) {
		pr_debug("Failed to get buildids: %d\n", errno);
		return -EINVAL;
	}
	strlist__for_each_entry(nd, bidlist) {
		pcache = probe_cache__new(nd->s);
		if (!pcache)
			continue;
		if (!list_empty(&pcache->entries)) {
			buf = build_id_cache__origname(nd->s);
			printf("%s (%s):\n", buf, nd->s);
			free(buf);
			probe_cache__show_entries(pcache, filter);
		}
		probe_cache__delete(pcache);
	}
	strlist__delete(bidlist);

	return 0;
}

static struct {
	const char *pattern;
	bool	avail;
	bool	checked;
} probe_type_table[] = {
#define DEFINE_TYPE(idx, pat, def_avail)	\
	[idx] = {.pattern = pat, .avail = (def_avail)}
	DEFINE_TYPE(PROBE_TYPE_U, "* u8/16/32/64,*", true),
	DEFINE_TYPE(PROBE_TYPE_S, "* s8/16/32/64,*", true),
	DEFINE_TYPE(PROBE_TYPE_X, "* x8/16/32/64,*", false),
	DEFINE_TYPE(PROBE_TYPE_STRING, "* string,*", true),
	DEFINE_TYPE(PROBE_TYPE_BITFIELD,
		    "* b<bit-width>@<bit-offset>/<container-size>", true),
};

bool probe_type_is_available(enum probe_type type)
{
	FILE *fp;
	char *buf = NULL;
	size_t len = 0;
	bool target_line = false;
	bool ret = probe_type_table[type].avail;

	if (type >= PROBE_TYPE_END)
		return false;
	/* We don't have to check the type which supported by default */
	if (ret || probe_type_table[type].checked)
		return ret;

	if (asprintf(&buf, "%s/README", tracing_path) < 0)
		return ret;

	fp = fopen(buf, "r");
	if (!fp)
		goto end;

	zfree(&buf);
	while (getline(&buf, &len, fp) > 0 && !ret) {
		if (!target_line) {
			target_line = !!strstr(buf, " type: ");
			if (!target_line)
				continue;
		} else if (strstr(buf, "\t          ") != buf)
			break;
		ret = strglobmatch(buf, probe_type_table[type].pattern);
	}
	/* Cache the result */
	probe_type_table[type].checked = true;
	probe_type_table[type].avail = ret;

	fclose(fp);
end:
	free(buf);

	return ret;
}

void free_sdt_list(struct list_head *sdt_events)
{
	struct sdt_event_list *tmp, *ptr;

	if (list_empty(sdt_events))
		return;
	list_for_each_entry_safe(tmp, ptr, sdt_events, list) {
		list_del(&tmp->list);
		free(tmp->event_info);
		free(tmp);
	}
}

static int alloc_exst_sdt_event(struct exst_sdt_event_list **esl)
{
	struct exst_sdt_event_list *tmp;

	tmp = zalloc(sizeof(*tmp));
	if (!tmp)
		return -ENOMEM;

	tmp->tev = zalloc(sizeof(struct probe_trace_event));
	if (!tmp->tev) {
		free(tmp);
		return -ENOMEM;
	}

	tmp->match = false;
	*esl = tmp;
	return 0;
}

static void free_exst_sdt_event(struct exst_sdt_event_list *esl)
{
	if (!esl)
		return;

	if (esl->tev) {
		free(esl->tev->args);
		free(esl->tev);
	}

	free(esl);
}

static void probe_file__free_exst_sdt_list(struct exst_sdt_event_list *esl)
{
	struct list_head *pos, *q;
	struct exst_sdt_event_list *tmp;

	list_for_each_safe(pos, q, &(esl->list)) {
		tmp = list_entry(pos, struct exst_sdt_event_list, list);
		free_exst_sdt_event(tmp);
		list_del(pos);
	}
}

/*
 * Look into uprobe_events file and prepare list of sdt events
 * whose probepoint is already registered.
 */
static int probe_file__get_exst_sdt_list(struct exst_sdt_event_list *esl)
{
	int fd, ret = 0;
	struct strlist *rawlist;
	struct str_node *ent;
	struct exst_sdt_event_list *tmp = NULL;

	fd = probe_file__open(PF_FL_RW | PF_FL_UPROBE);
	if (fd < 0)
		return fd;

	rawlist = probe_file__get_rawlist(fd);

	strlist__for_each_entry(ent, rawlist) {
		ret = alloc_exst_sdt_event(&tmp);
		if (ret < 0)
			goto error;

		ret = parse_probe_trace_command(ent->s, tmp->tev);
		if (ret < 0) {
			free_exst_sdt_event(tmp);
			goto error;
		}

		if (!strncmp(tmp->tev->group, "sdt_", 4))
			list_add_tail(&(tmp->list), &(esl->list));
		else
			free_exst_sdt_event(tmp);
	}
	return 0;

error:
	probe_file__free_exst_sdt_list(esl);
	return ret;
}

/*
 * Remove ith tev from pev->tevs list and shift remaining
 * tevs(i+1 to pev->ntevs) one step.
 */
static void shift_pev(struct perf_probe_event *pev, int i)
{
	int j;

	free(pev->tevs[i].event);
	free(pev->tevs[i].group);
	free(pev->tevs[i].args);
	free(pev->tevs[i].point.realname);
	free(pev->tevs[i].point.symbol);
	free(pev->tevs[i].point.module);

	/*
	 * If ith element is last element, no need to shift,
	 * just decrement pev->ntevs.
	 */
	if (i == pev->ntevs - 1)
		goto ret;

	for (j = i; j < pev->ntevs - 1; j++) {
		pev->tevs[j].event          = pev->tevs[j + 1].event;
		pev->tevs[j].group          = pev->tevs[j + 1].group;
		pev->tevs[j].nargs          = pev->tevs[j + 1].nargs;
		pev->tevs[j].uprobes        = pev->tevs[j + 1].uprobes;
		pev->tevs[j].args           = pev->tevs[j + 1].args;
		pev->tevs[j].point.realname = pev->tevs[j + 1].point.realname;
		pev->tevs[j].point.symbol   = pev->tevs[j + 1].point.symbol;
		pev->tevs[j].point.module   = pev->tevs[j + 1].point.module;
		pev->tevs[j].point.offset   = pev->tevs[j + 1].point.offset;
		pev->tevs[j].point.address  = pev->tevs[j + 1].point.address;
		pev->tevs[j].point.retprobe = pev->tevs[j + 1].point.retprobe;
	}

ret:
	pev->ntevs--;
}

/* Compare address and filename */
static bool is_sdt_match(struct probe_trace_event *tev1,
			 struct probe_trace_event *tev2)
{
	return (tev1->point.address == tev2->point.address &&
		!(strcmp(tev1->point.module, tev2->point.module)));
}

/*
 * Filter out all those pev->tevs which already exists in uprobe_events.
 * Return 'true' if any matching entry found otherwise return 'false'.
 */
static bool filter_exst_sdt_events_from_pev(struct perf_probe_event *pev,
					    struct exst_sdt_event_list *esl)
{
	int i;
	bool ret = false;
	struct exst_sdt_event_list *tmp;

	list_for_each_entry(tmp, &(esl->list), list) {
		for (i = 0; i < pev->ntevs; i++) {
			if (is_sdt_match(&(pev->tevs[i]), tmp->tev)) {
				shift_pev(pev, i);
				tmp->match = true;
				ret = true;
			}
		}
	}

	return ret;
}

static int list_add_sdt_event(struct list_head *sdt_events,
			      bool existing_event,
			      struct probe_trace_event *tev)
{
	struct sdt_event_list *tmp;

	tmp = zalloc(sizeof(*tmp));
	if (!tmp)
		return -ENOMEM;

	tmp->existing_event = existing_event;

	INIT_LIST_HEAD(&tmp->list);
	tmp->event_info = zalloc(MAX_EVENT_LENGTH * sizeof(char));
	if (!tmp->event_info) {
		free(tmp);
		return -ENOMEM;
	}

	snprintf(tmp->event_info, strlen(tev->group) + strlen(tev->event) + 2,
		 "%s:%s", tev->group, tev->event);

	list_add(&tmp->list, sdt_events);

	return 0;
}

static void print_exst_sdt_events(struct exst_sdt_event_list *tmp)
{
	static bool msg_head;

	if (!msg_head) {
		pr_info("Matching event(s) from uprobe_events:\n");
		msg_head = true;
	}

	pr_info("   %s:%s  0x%" PRIx64 "@%s\n", tmp->tev->group,
		tmp->tev->event, tmp->tev->point.address,
		tmp->tev->point.module);
}

static void print_exst_sdt_event_footer(void)
{
	pr_info("Use 'perf probe -d <event>' to delete event(s).\n\n");
}

/*
 * If there is entry with the same name in uprobe_events, record it.
 * Return value  0: no error, not found
 *              <0: error
 *              >0: found
 */
static int probe_file__add_exst_sdt_event(struct exst_sdt_event_list *esl,
					  struct list_head *sdt_events,
					  struct perf_probe_event *pev)
{
	struct exst_sdt_event_list *tmp;
	int ret = 0;

	list_for_each_entry(tmp, &(esl->list), list) {
		if (strcmp(tmp->tev->group, pev->group) ||
		    strcmp(tmp->tev->event, pev->event))
			continue;

		tmp->match = true;

		ret = list_add_sdt_event(sdt_events, true, tmp->tev);
		if (ret < 0)
			return ret;

		print_exst_sdt_events(tmp);
		print_exst_sdt_event_footer();
		if (pev->ntevs > 1)
			pr_warning("Warning: Found %d events from probe-cache with name '%s:%s'.\n"
				"\t Since probe point already exists with this name, recording only 1 event.\n"
				"Hint: Please use 'perf probe -d %s:%s*' to allow record on all events.\n\n",
				pev->ntevs, pev->group, pev->event, pev->group, pev->event);
		return 1;
	}
	return 0;
}

int add_sdt_event(char *event, struct list_head *sdt_events,
		  struct parse_events_error *err)
{
	struct perf_probe_event *pev;
	int ret = 0, i, ctr = 0, found = 0, exst_ctr = 0;
	char *str;
	struct exst_sdt_event_list *tmp;
	struct exst_sdt_event_list esl;
	bool filter;

	pev = zalloc(sizeof(*pev));
	if (!pev)
		return -ENOMEM;

	pev->sdt = true;
	pev->uprobes = true;

	str = strdup(event);
	if (!str)
		return -ENOMEM;

	/*
	 * Parse event to find the group name and event name of
	 * the sdt event.
	 */
	ret = parse_perf_probe_event_name(&event, pev);
	if (ret) {
		pr_err("Error in parsing sdt event %s\n", str);
		free(pev);
		goto free_str;
	}

	probe_conf.max_probes = MAX_PROBES;
	probe_conf.force_add = 1;

	/*
	 * Find the sdt event from the cache. We deliberately check failure
	 * of this function after checking entries in uprobe_events. Because,
	 * we may find matching entry from uprobe_events and in that case we
	 * should continue recording that event.
	 */
	pev->ntevs = find_sdt_events_from_cache(pev, &pev->tevs);

	/* Prepare list of existing sdt events from uprobe_events */
	INIT_LIST_HEAD(&esl.list);
	ret = probe_file__get_exst_sdt_list(&esl);
	if (ret < 0)
		goto free_str;

	/* If there is entry with the same name in uprobe_events, record it. */
	found = probe_file__add_exst_sdt_event(&esl, sdt_events, pev);
	if (found) {
		ret = (found > 0) ? 0 : found;
		goto free_str;
	}

	/* Reaching here means no matching entry found in uprobe_events. */
	filter = filter_exst_sdt_events_from_pev(pev, &esl);
	if (!filter && pev->ntevs == 0) {
		pr_err("%s:%s not found in the cache\n", pev->group,
			pev->event);
		ret = -EINVAL;
		goto free_str;
	} else if (pev->ntevs < 0) {
		err->str = strdup("Cache lookup failed.\n");
		ret = pev->ntevs;
		goto free_str;
	}

	/* Create probe points for new events. */
	ret = apply_perf_probe_events(pev, 1);
	if (ret) {
		pr_err("Error in adding SDT event : %s\n", event);
		goto free_str;
	}

	/* Add existing event names to "sdt_events" list */
	list_for_each_entry(tmp, &(esl.list), list) {
		if (!tmp->match)
			continue;

		ret = list_add_sdt_event(sdt_events, true, tmp->tev);
		if (ret < 0)
			goto free_str;
		print_exst_sdt_events(tmp);
		ctr++;
		exst_ctr++;
	}
	if (exst_ctr)
		print_exst_sdt_event_footer();

	/* Add new event names to "sdt_events" list */
	for (i = 0; i < pev->ntevs; i++) {
		ret = list_add_sdt_event(sdt_events, false, &(pev->tevs[i]));
		if (ret < 0)
			goto free_str;

		ctr++;
	}

	/* Print warning for multiple events */
	if (ctr > 1)
		pr_warning("Warning: Recording on %d occurrences of %s:%s\n",
			   ctr, pev->group, pev->event);

free_str:
	/*
	 * User may ask for multiple events in the same record command like,
	 *    perf record -a -e sdt_lib1:* -e sdt_a:b
	 *
	 * If sdt_lib1:* events are already added and there is some failure
	 * for sdt_a:b, we need to clean sdt_lib1:* events from
	 * record.sdt_event_list
	 */
	if (ret < 0)
		sdt_event_list__remove();

	free(str);
	probe_file__free_exst_sdt_list(&esl);
	cleanup_perf_probe_events(pev, 1);
	return ret;
}
