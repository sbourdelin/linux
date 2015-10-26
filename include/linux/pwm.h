#ifndef __LINUX_PWM_H
#define __LINUX_PWM_H

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/of.h>

struct pwm_device;
struct seq_file;

#if IS_ENABLED(CONFIG_PWM)
/*
 * pwm_request - request a PWM device
 */
struct pwm_device *pwm_request(int pwm_id, const char *label);

/*
 * pwm_free - free a PWM device
 */
void pwm_free(struct pwm_device *pwm);

/*
 * pwm_config - change a PWM device configuration
 */
int pwm_config(struct pwm_device *pwm, int duty_ns,
	       int period_ns, unsigned int pulse_count);

/*
 * pwm_pulse_done - notify the PWM framework that pulse_count pulses are done
 */
void pwm_pulse_done(struct pwm_device *pwm);

/*
 * pwm_enable - start a PWM output toggling
 */
int pwm_enable(struct pwm_device *pwm);

/*
 * pwm_disable - stop a PWM output toggling
 */
void pwm_disable(struct pwm_device *pwm);
#else
static inline struct pwm_device *pwm_request(int pwm_id, const char *label)
{
	return ERR_PTR(-ENODEV);
}

static inline void pwm_free(struct pwm_device *pwm)
{
}

static inline int pwm_config(struct pwm_device *pwm, int duty_ns,
			     int period_ns, unsigned int pulse_count)
{
	return -EINVAL;
}

static inline int pwm_enable(struct pwm_device *pwm)
{
	return -EINVAL;
}

static inline int pwm_pulse_done(struct pwm_device *pwm)
{
	return -EINVAL;
}

static inline void pwm_disable(struct pwm_device *pwm)
{
}
#endif

struct pwm_chip;

/**
 * enum pwm_polarity - polarity of a PWM signal
 * @PWM_POLARITY_NORMAL: a high signal for the duration of the duty-
 * cycle, followed by a low signal for the remainder of the pulse
 * period
 * @PWM_POLARITY_INVERSED: a low signal for the duration of the duty-
 * cycle, followed by a high signal for the remainder of the pulse
 * period
 */
enum pwm_polarity {
	PWM_POLARITY_NORMAL,
	PWM_POLARITY_INVERSED,
};

enum {
	PWMF_REQUESTED = BIT(0),
	PWMF_ENABLED = BIT(1),
	PWMF_EXPORTED = BIT(2),
	PWMF_PULSING = BIT(3),
};

/**
 * struct pwm_device - PWM channel object
 * @label: name of the PWM device
 * @flags: flags associated with the PWM device
 * @hwpwm: per-chip relative index of the PWM device
 * @pwm: global index of the PWM device
 * @chip: PWM chip providing this PWM device
 * @chip_data: chip-private data associated with the PWM device
 * @period: period of the PWM signal (in nanoseconds)
 * @duty_cycle: duty cycle of the PWM signal (in nanoseconds)
 * @polarity: polarity of the PWM signal
 * @pulse_count: number of PWM pulses to toggle
 * @pulse_count_max: maximum number of pulses that can be set to pulse
 */
struct pwm_device {
	const char *label;
	unsigned long flags;
	unsigned int hwpwm;
	unsigned int pwm;
	struct pwm_chip *chip;
	void *chip_data;

	unsigned int period;
	unsigned int duty_cycle;
	enum pwm_polarity polarity;
	unsigned int pulse_count;
	unsigned int pulse_count_max;
};

static inline bool pwm_is_enabled(const struct pwm_device *pwm)
{
	return test_bit(PWMF_ENABLED, &pwm->flags);
}

static inline bool pwm_is_pulsing(const struct pwm_device *pwm)
{
	return test_bit(PWMF_ENABLED | PWMF_PULSING, &pwm->flags);
}

static inline void pwm_set_period(struct pwm_device *pwm, unsigned int period)
{
	if (pwm)
		pwm->period = period;
}

static inline unsigned int pwm_get_period(const struct pwm_device *pwm)
{
	return pwm ? pwm->period : 0;
}

static inline void pwm_set_duty_cycle(struct pwm_device *pwm, unsigned int duty)
{
	if (pwm)
		pwm->duty_cycle = duty;
}

static inline unsigned int pwm_get_duty_cycle(const struct pwm_device *pwm)
{
	return pwm ? pwm->duty_cycle : 0;
}

/*
 * pwm_set_polarity - configure the polarity of a PWM signal
 */
int pwm_set_polarity(struct pwm_device *pwm, enum pwm_polarity polarity);

static inline enum pwm_polarity pwm_get_polarity(const struct pwm_device *pwm)
{
	return pwm ? pwm->polarity : PWM_POLARITY_NORMAL;
}

/*
 * pwm_set_pulse_count - configure the number of pulses of a pwm_pulse
 */
static inline void pwm_set_pulse_count(struct pwm_device *pwm,
				       unsigned int pulse_count)
{
	if (pwm)
		pwm->pulse_count = 0;
}

/*
 * pwm_get_pulse_count - retrieve the number of pules to pulse of a pwm_pulse
 */
static inline unsigned int pwm_get_pulse_count(const struct pwm_device *pwm)
{
	return pwm ? pwm->pulse_count : 0;
}

/*
 * pwm_get_pulse_count_max - retrieve the maximum number of pulses
 */
static inline unsigned int pwm_get_pulse_count_max(const struct pwm_device *pwm)
{
	return pwm ? pwm->pulse_count_max : 0;
}

/*
 * pwm_set_pulse_count_max - set the maximum number of pulses
 */
static inline void pwm_set_pulse_count_max(struct pwm_device *pwm,
					   unsigned int pulse_count_max)
{
	if (pwm)
		pwm->pulse_count_max = pulse_count_max;
}

/**
 * struct pwm_ops - PWM controller operations
 * @request: optional hook for requesting a PWM
 * @free: optional hook for freeing a PWM
 * @config: configure duty cycles and period length for this PWM
 * @set_polarity: configure the polarity of this PWM
 * @enable: enable PWM output toggling
 * @disable: disable PWM output toggling
 * @dbg_show: optional routine to show contents in debugfs
 * @owner: helps prevent removal of modules exporting active PWMs
 */
struct pwm_ops {
	int (*request)(struct pwm_chip *chip, struct pwm_device *pwm);
	void (*free)(struct pwm_chip *chip, struct pwm_device *pwm);
	int (*config)(struct pwm_chip *chip, struct pwm_device *pwm,
		      int duty_ns, int period_ns, unsigned int pulse_count);
	int (*set_polarity)(struct pwm_chip *chip, struct pwm_device *pwm,
			    enum pwm_polarity polarity);
	int (*enable)(struct pwm_chip *chip, struct pwm_device *pwm);
	void (*disable)(struct pwm_chip *chip, struct pwm_device *pwm);
#ifdef CONFIG_DEBUG_FS
	void (*dbg_show)(struct pwm_chip *chip, struct seq_file *s);
#endif
	struct module *owner;
};

/**
 * struct pwm_chip - abstract a PWM controller
 * @dev: device providing the PWMs
 * @list: list node for internal use
 * @ops: callbacks for this PWM controller
 * @base: number of first PWM controlled by this chip
 * @npwm: number of PWMs controlled by this chip
 * @pwms: array of PWM devices allocated by the framework
 * @of_xlate: request a PWM device given a device tree PWM specifier
 * @of_pwm_n_cells: number of cells expected in the device tree PWM specifier
 * @can_sleep: must be true if the .config(), .enable() or .disable()
 *             operations may sleep
 */
struct pwm_chip {
	struct device *dev;
	struct list_head list;
	const struct pwm_ops *ops;
	int base;
	unsigned int npwm;

	struct pwm_device *pwms;

	struct pwm_device * (*of_xlate)(struct pwm_chip *pc,
					const struct of_phandle_args *args);
	unsigned int of_pwm_n_cells;
	bool can_sleep;
};

#if IS_ENABLED(CONFIG_PWM)
int pwm_set_chip_data(struct pwm_device *pwm, void *data);
void *pwm_get_chip_data(struct pwm_device *pwm);

int pwmchip_add_with_polarity(struct pwm_chip *chip,
			      enum pwm_polarity polarity);
int pwmchip_add(struct pwm_chip *chip);
int pwmchip_remove(struct pwm_chip *chip);
struct pwm_device *pwm_request_from_chip(struct pwm_chip *chip,
					 unsigned int index,
					 const char *label);

struct pwm_device *of_pwm_xlate_with_flags(struct pwm_chip *pc,
		const struct of_phandle_args *args);

struct pwm_device *pwm_get(struct device *dev, const char *con_id);
struct pwm_device *of_pwm_get(struct device_node *np, const char *con_id);
void pwm_put(struct pwm_device *pwm);

struct pwm_device *devm_pwm_get(struct device *dev, const char *con_id);
struct pwm_device *devm_of_pwm_get(struct device *dev, struct device_node *np,
				   const char *con_id);
void devm_pwm_put(struct device *dev, struct pwm_device *pwm);

bool pwm_can_sleep(struct pwm_device *pwm);
#else
static inline int pwm_set_chip_data(struct pwm_device *pwm, void *data)
{
	return -EINVAL;
}

static inline void *pwm_get_chip_data(struct pwm_device *pwm)
{
	return NULL;
}

static inline int pwmchip_add(struct pwm_chip *chip)
{
	return -EINVAL;
}

static inline int pwmchip_add_inversed(struct pwm_chip *chip)
{
	return -EINVAL;
}

static inline int pwmchip_remove(struct pwm_chip *chip)
{
	return -EINVAL;
}

static inline struct pwm_device *pwm_request_from_chip(struct pwm_chip *chip,
						       unsigned int index,
						       const char *label)
{
	return ERR_PTR(-ENODEV);
}

static inline struct pwm_device *pwm_get(struct device *dev,
					 const char *consumer)
{
	return ERR_PTR(-ENODEV);
}

static inline struct pwm_device *of_pwm_get(struct device_node *np,
					    const char *con_id)
{
	return ERR_PTR(-ENODEV);
}

static inline void pwm_put(struct pwm_device *pwm)
{
}

static inline struct pwm_device *devm_pwm_get(struct device *dev,
					      const char *consumer)
{
	return ERR_PTR(-ENODEV);
}

static inline struct pwm_device *devm_of_pwm_get(struct device *dev,
						 struct device_node *np,
						 const char *con_id)
{
	return ERR_PTR(-ENODEV);
}

static inline void devm_pwm_put(struct device *dev, struct pwm_device *pwm)
{
}

static inline bool pwm_can_sleep(struct pwm_device *pwm)
{
	return false;
}
#endif

struct pwm_lookup {
	struct list_head list;
	const char *provider;
	unsigned int index;
	const char *dev_id;
	const char *con_id;
	unsigned int period;
	enum pwm_polarity polarity;
};

#define PWM_LOOKUP(_provider, _index, _dev_id, _con_id, _period, _polarity) \
	{						\
		.provider = _provider,			\
		.index = _index,			\
		.dev_id = _dev_id,			\
		.con_id = _con_id,			\
		.period = _period,			\
		.polarity = _polarity			\
	}

#if IS_ENABLED(CONFIG_PWM)
void pwm_add_table(struct pwm_lookup *table, size_t num);
void pwm_remove_table(struct pwm_lookup *table, size_t num);
#else
static inline void pwm_add_table(struct pwm_lookup *table, size_t num)
{
}

static inline void pwm_remove_table(struct pwm_lookup *table, size_t num)
{
}
#endif

#ifdef CONFIG_PWM_SYSFS
void pwmchip_sysfs_export(struct pwm_chip *chip);
void pwmchip_sysfs_unexport(struct pwm_chip *chip);
#else
static inline void pwmchip_sysfs_export(struct pwm_chip *chip)
{
}

static inline void pwmchip_sysfs_unexport(struct pwm_chip *chip)
{
}
#endif /* CONFIG_PWM_SYSFS */

#endif /* __LINUX_PWM_H */
