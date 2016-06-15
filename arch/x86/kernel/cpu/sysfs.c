#include <linux/sysfs.h>
#include <linux/device.h>

static ssize_t
energy_policy_pref_hint_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	u64 epb;

	rdmsrl(MSR_IA32_ENERGY_PERF_BIAS, epb);

	return sprintf(buf, "%d\n", (unsigned int)(epb & 0xFULL));
}

static ssize_t
energy_policy_pref_hint_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	u32 val;
	u64 epb;

	if (kstrtou32(buf, 10, &val) < 0)
		return -EINVAL;

	if (val > 15)
		return -EINVAL;

	rdmsrl(MSR_IA32_ENERGY_PERF_BIAS, epb);

	if ((epb & 0xf) == val)
		return count;

	wrmsrl(MSR_IA32_ENERGY_PERF_BIAS, (epb & ~0xFULL) | val);

	return count;
}

static DEVICE_ATTR_RW(energy_policy_pref_hint);

static struct attribute *cpu_attrs[] = {
	&dev_attr_energy_policy_pref_hint.attr,
	NULL,
};

static struct attribute_group cpu_attr_group = {
	.attrs = cpu_attrs,
};

const struct attribute_group * arch_get_cpu_group(void)
{
	if (static_cpu_has(X86_FEATURE_EPB))
		return &cpu_attr_group;

	return NULL;
}
