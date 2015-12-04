#ifndef __PERF_UTIL_H
#define __PERF_UTIL_H

#include "compat-util.h"

#include "config.h"
#include "ctype.h"
#include "lzma.h"
#include "srcline.h"
#include "strbuf.h"
#include "string.h"
#include "term.h"
#include "usage.h"
#include "wrapper.h"
#include "zlib.h"

int parse_nsec_time(const char *str, u64 *ptime);

int mkdir_p(char *path, mode_t mode);
int rm_rf(char *path);
int copyfile(const char *from, const char *to);
int copyfile_mode(const char *from, const char *to, mode_t mode);
int copyfile_offset(int fromfd, loff_t from_ofs, int tofd, loff_t to_ofs, u64 size);

unsigned long convert_unit(unsigned long value, char *unit);
ssize_t readn(int fd, void *buf, size_t n);
ssize_t writen(int fd, void *buf, size_t n);

struct perf_event_attr;

void event_attr_init(struct perf_event_attr *attr);

size_t hex_width(u64 v);
int hex2u64(const char *ptr, u64 *val);

void dump_stack(void);
void sighandler_dump_stack(int sig);

extern unsigned int page_size;
extern int cacheline_size;

struct parse_tag {
	char tag;
	int mult;
};

unsigned long parse_tag_value(const char *str, struct parse_tag *tags);

static inline int path__join(char *bf, size_t size,
			     const char *path1, const char *path2)
{
	return scnprintf(bf, size, "%s%s%s", path1, path1[0] ? "/" : "", path2);
}

static inline int path__join3(char *bf, size_t size,
			      const char *path1, const char *path2,
			      const char *path3)
{
	return scnprintf(bf, size, "%s%s%s%s%s",
			 path1, path1[0] ? "/" : "",
			 path2, path2[0] ? "/" : "", path3);
}

int filename__read_str(const char *filename, char **buf, size_t *sizep);
int perf_event_paranoid(void);

void mem_bswap_64(void *src, int byte_size);
void mem_bswap_32(void *src, int byte_size);

const char *get_filename_for_perf_kvm(void);
bool find_process(const char *name);

int get_stack_size(const char *str, unsigned long *_size);

int fetch_kernel_version(unsigned int *puint,
			 char *str, size_t str_sz);
#define KVER_VERSION(x)		(((x) >> 16) & 0xff)
#define KVER_PATCHLEVEL(x)	(((x) >> 8) & 0xff)
#define KVER_SUBLEVEL(x)	((x) & 0xff)
#define KVER_FMT	"%d.%d.%d"
#define KVER_PARAM(x)	KVER_VERSION(x), KVER_PATCHLEVEL(x), KVER_SUBLEVEL(x)

#endif /* __PERF_UTIL_H */
