#ifndef __LINUX_PWRSEQ_H
#define __LINUX_PWRSEQ_H

#include <linux/of.h>

#define PWRSEQ_MAX_CLKS		3

struct pwrseq {
	char *name;
	struct list_head node;
	int (*get)(struct device_node *np, struct pwrseq *p);
	int (*on)(struct device_node *np, struct pwrseq *p);
	void (*off)(struct pwrseq *p);
	void (*put)(struct pwrseq *p);
	void (*free)(struct pwrseq *p);
};

#if IS_ENABLED(CONFIG_POWER_SEQUENCE)
int pwrseq_get(struct device_node *np, struct pwrseq *p);
int pwrseq_on(struct device_node *np, struct pwrseq *p);
void pwrseq_off(struct pwrseq *p);
void pwrseq_put(struct pwrseq *p);
void pwrseq_free(struct pwrseq *p);
#else
static inline int pwrseq_get(struct device_node *np, struct pwrseq *p)
{
	return 0;
}
static inline int pwrseq_on(struct device_node *np, struct pwrseq *p)
{
	return 0;
}
static inline void pwrseq_off(struct pwrseq *p) {}
static inline void pwrseq_put(struct pwrseq *p) {}
static inline void pwrseq_free(struct pwrseq *p) {}
#endif /* CONFIG_POWER_SEQUENCE */

#if IS_ENABLED(CONFIG_PWRSEQ_GENERIC)
struct pwrseq *pwrseq_alloc_generic(void);
#else
static inline struct pwrseq *pwrseq_alloc_generic(void)
{
	return NULL;
}
#endif /* CONFIG_PWRSEQ_GENERIC */

#endif  /* __LINUX_PWRSEQ_H */
