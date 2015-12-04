#ifndef __PERF_SRCLINE_H
#define __PERF_SRCLINE_H

#define SRCLINE_UNKNOWN  ((char *) "??:0")

struct dso;
struct symbol;

extern bool srcline_full_filename;
char *get_srcline(struct dso *dso, u64 addr, struct symbol *sym,
		  bool show_sym);
char *__get_srcline(struct dso *dso, u64 addr, struct symbol *sym,
		  bool show_sym, bool unwind_inlines);
void free_srcline(char *srcline);

#endif /* __PERF_SRCLINE_H */
