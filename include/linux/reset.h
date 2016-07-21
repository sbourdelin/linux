#ifndef _LINUX_RESET_H_
#define _LINUX_RESET_H_

#include <linux/device.h>

struct reset_control;

#ifdef CONFIG_RESET_CONTROLLER

int reset_control_reset(struct reset_control *rstc);
int reset_control_assert(struct reset_control *rstc);
int reset_control_deassert(struct reset_control *rstc);
int reset_control_status(struct reset_control *rstc);

struct reset_control *__of_reset_control_get(struct device_node *node,
				     const char *id, int index, int shared);
void reset_control_put(struct reset_control *rstc);
struct reset_control *__devm_reset_control_get(struct device *dev,
				     const char *id, int index, int shared);

int __must_check device_reset(struct device *dev);

static inline int device_reset_optional(struct device *dev)
{
	return device_reset(dev);
}

#else

static inline int reset_control_reset(struct reset_control *rstc)
{
	WARN_ON(1);
	return 0;
}

static inline int reset_control_assert(struct reset_control *rstc)
{
	WARN_ON(1);
	return 0;
}

static inline int reset_control_deassert(struct reset_control *rstc)
{
	WARN_ON(1);
	return 0;
}

static inline int reset_control_status(struct reset_control *rstc)
{
	WARN_ON(1);
	return 0;
}

static inline void reset_control_put(struct reset_control *rstc)
{
	WARN_ON(1);
}

static inline int __must_check device_reset(struct device *dev)
{
	WARN_ON(1);
	return -ENOTSUPP;
}

static inline int device_reset_optional(struct device *dev)
{
	return -ENOTSUPP;
}

static inline struct reset_control *__of_reset_control_get(
					struct device_node *node,
					const char *id, int index, int shared)
{
	return ERR_PTR(-ENOTSUPP);
}

static inline struct reset_control *__devm_reset_control_get(
					struct device *dev,
					const char *id, int index, int shared)
{
	return ERR_PTR(-ENOTSUPP);
}

#endif /* CONFIG_RESET_CONTROLLER */

#define GENERATE_RESET_CONTROL_GET_FUNCS(optional, shared, suffix)	\
static inline struct reset_control * __must_check			\
reset_control_get_ ## suffix(struct device *dev, const char *id)	\
{									\
	WARN_ON(!IS_ENABLED(CONFIG_RESET_CONTROLLER) && !optional);	\
	return __of_reset_control_get(dev ? dev->of_node : NULL,	\
				      id, 0, shared);			\
}									\
									\
static inline struct reset_control * __must_check			\
reset_control_get_ ## suffix ## _by_index(struct device *dev, int index)\
{									\
	WARN_ON(!IS_ENABLED(CONFIG_RESET_CONTROLLER) && !optional);	\
	return __of_reset_control_get(dev ? dev->of_node : NULL,	\
				      NULL, index, shared);		\
}									\
									\
static inline struct reset_control * __must_check			\
of_reset_control_get_ ## suffix(struct device_node *node, const char *id)\
{									\
	WARN_ON(!IS_ENABLED(CONFIG_RESET_CONTROLLER) && !optional);	\
	return __of_reset_control_get(node, id, 0, shared);		\
}									\
									\
static inline struct reset_control * __must_check			\
of_reset_control_get_ ## suffix ## _by_index(struct device_node *node,	\
					     int index)			\
{									\
	WARN_ON(!IS_ENABLED(CONFIG_RESET_CONTROLLER) && !optional);	\
	return __of_reset_control_get(node, NULL, index, shared);	\
}									\
									\
static inline struct reset_control * __must_check			\
devm_reset_control_get_ ## suffix(struct device *dev, const char *id)	\
{									\
	WARN_ON(!IS_ENABLED(CONFIG_RESET_CONTROLLER) && !optional);	\
	return __devm_reset_control_get(dev, id, 0, shared);		\
}									\
									\
static inline struct reset_control * __must_check			\
devm_reset_control_get_ ## suffix ## _by_index(struct device *dev, int index)\
{									\
	WARN_ON(!IS_ENABLED(CONFIG_RESET_CONTROLLER) && !optional);	\
	return __devm_reset_control_get(dev, 0, index, shared);		\
}

GENERATE_RESET_CONTROL_GET_FUNCS(0, 0, exclusive)
GENERATE_RESET_CONTROL_GET_FUNCS(0, 1, shared)
GENERATE_RESET_CONTROL_GET_FUNCS(1, 0, optional_exclusive)
GENERATE_RESET_CONTROL_GET_FUNCS(1, 1, optional_shared)

/*
 * TEMPORARY calls to use during transition:
 *
 *   of_reset_control_get() => of_reset_control_get_exclusive()
 *
 * These inline function calls will be removed once all consumers
 * have been moved over to the new explicit API.
 */
static inline struct reset_control *reset_control_get(
				struct device *dev, const char *id)
{
	return reset_control_get_exclusive(dev, id);
}

static inline struct reset_control *reset_control_get_optional(
					struct device *dev, const char *id)
{
	return reset_control_get_optional_exclusive(dev, id);
}

static inline struct reset_control *of_reset_control_get(
				struct device_node *node, const char *id)
{
	return of_reset_control_get_exclusive(node, id);
}

static inline struct reset_control *of_reset_control_get_by_index(
				struct device_node *node, int index)
{
	return of_reset_control_get_exclusive_by_index(node, index);
}

static inline struct reset_control *devm_reset_control_get(
				struct device *dev, const char *id)
{
	return devm_reset_control_get_exclusive(dev, id);
}

static inline struct reset_control *devm_reset_control_get_optional(
				struct device *dev, const char *id)
{
	return devm_reset_control_get_optional_exclusive(dev, id);

}

static inline struct reset_control *devm_reset_control_get_by_index(
				struct device *dev, int index)
{
	return devm_reset_control_get_exclusive_by_index(dev, index);
}
#endif
