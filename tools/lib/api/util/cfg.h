#ifndef __API_UTIL_CONFIG_H
#define __API_UTIL_CONFIG_H

struct util_cfg {
	const char *prefix;
	const char *exec_name;
	const char *exec_path;
	const char *exec_path_env;
	const char *pager_env;
	void (*exit_browser)(void);
};

extern struct util_cfg util_cfg;

#endif /* #define __API_UTIL_CONFIG_H */
