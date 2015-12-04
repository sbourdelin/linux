#ifndef __PERF_CONFIG_H
#define __PERF_CONFIG_H

#define PERF_GTK_DSO  "libperf-gtk.so"

extern char buildid_dir[];
extern void set_buildid_dir(const char *dir);

typedef int (*config_fn_t)(const char *, const char *, void *);
extern int perf_default_config(const char *, const char *, void *);
extern int perf_config(config_fn_t fn, void *);
extern int perf_config_int(const char *, const char *);
extern u64 perf_config_u64(const char *, const char *);
extern int perf_config_bool(const char *, const char *);
extern int config_error_nonbool(const char *);
extern const char *perf_config_dirname(const char *, const char *);

#endif /* __PERF_CONFIG_H */
