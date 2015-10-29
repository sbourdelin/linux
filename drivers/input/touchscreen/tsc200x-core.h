#ifndef _TSC200X_CORE_H
#define _TSC200X_CORE_H

extern const struct regmap_config tsc2005_regmap_config;

int __maybe_unused tsc200x_suspend(struct device *dev);
int __maybe_unused tsc200x_resume(struct device *dev);

int tsc200x_probe(struct device *dev, int irq, __u16 bustype,
			 struct regmap *regmap);
int tsc200x_remove(struct device *dev);

#endif
