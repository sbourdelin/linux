#ifndef __PERF_ALIAS_H
#define __PERF_ALIAS_H

char *alias_lookup(const char *alias);
int split_cmdline(char *cmdline, const char ***argv);

#endif /* __PERF_ALIAS_H */
