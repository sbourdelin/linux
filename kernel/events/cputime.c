#include <linux/kernel_stat.h>
#include <linux/sched.h>
#include <linux/perf_event.h>

enum perf_cputime_id {
	PERF_CPUTIME_USER,
	PERF_CPUTIME_NICE,
	PERF_CPUTIME_SYSTEM,
	PERF_CPUTIME_SOFTIRQ,
	PERF_CPUTIME_IRQ,
	PERF_CPUTIME_IDLE,
	PERF_CPUTIME_IOWAIT,
	PERF_CPUTIME_STEAL,
	PERF_CPUTIME_GUEST,
	PERF_CPUTIME_GUEST_NICE,
	PERF_CPUTIME_MAX,
};

static enum cpu_usage_stat map[PERF_CPUTIME_MAX] = {
	[PERF_CPUTIME_USER]		= CPUTIME_USER,
	[PERF_CPUTIME_NICE]		= CPUTIME_NICE,
	[PERF_CPUTIME_SYSTEM]		= CPUTIME_SYSTEM,
	[PERF_CPUTIME_SOFTIRQ]		= CPUTIME_SOFTIRQ,
	[PERF_CPUTIME_IRQ]		= CPUTIME_IRQ,
	[PERF_CPUTIME_IDLE]		= CPUTIME_IDLE,
	[PERF_CPUTIME_IOWAIT]		= CPUTIME_IOWAIT,
	[PERF_CPUTIME_STEAL]		= CPUTIME_STEAL,
	[PERF_CPUTIME_GUEST]		= CPUTIME_GUEST,
	[PERF_CPUTIME_GUEST_NICE]	= CPUTIME_GUEST_NICE,
};

PMU_FORMAT_ATTR(event, "config:0-63");

static struct attribute *cputime_format_attrs[] = {
	&format_attr_event.attr,
	NULL,
};

static struct attribute_group cputime_format_attr_group = {
	.name = "format",
	.attrs = cputime_format_attrs,
};

static ssize_t
cputime_event_attr_show(struct device *dev, struct device_attribute *attr,
		    char *page)
{
	struct perf_pmu_events_attr *pmu_attr =
		container_of(attr, struct perf_pmu_events_attr, attr);

	return sprintf(page, "event=%llu\n", pmu_attr->id);
}

#define __A(__n, __e)					\
	PMU_EVENT_ATTR(__n, cputime_attr_##__n,		\
		       __e, cputime_event_attr_show);	\
	PMU_EVENT_ATTR_STRING(__n.unit,			\
			cputime_attr_##__n##_unit, "ns");

__A(user,	PERF_CPUTIME_USER)
__A(nice,	PERF_CPUTIME_NICE)
__A(system,	PERF_CPUTIME_SYSTEM)
__A(softirq,	PERF_CPUTIME_SOFTIRQ)
__A(irq,	PERF_CPUTIME_IRQ)
__A(idle,	PERF_CPUTIME_IDLE)
__A(iowait,	PERF_CPUTIME_IOWAIT)
__A(steal,	PERF_CPUTIME_STEAL)
__A(guest,	PERF_CPUTIME_GUEST)
__A(guest_nice,	PERF_CPUTIME_GUEST_NICE)

#undef __A

static struct attribute *cputime_events_attrs[] = {
#define __A(__n)				\
	&cputime_attr_##__n.attr.attr,		\
	&cputime_attr_##__n##_unit.attr.attr,

	__A(user)
	__A(nice)
	__A(system)
	__A(softirq)
	__A(irq)
	__A(idle)
	__A(iowait)
	__A(steal)
	__A(guest)
	__A(guest_nice)

	NULL,

#undef __A
};

static struct attribute_group cputime_events_attr_group = {
	.name = "events",
	.attrs = cputime_events_attrs,
};

static const struct attribute_group *cputime_attr_groups[] = {
	&cputime_format_attr_group,
	&cputime_events_attr_group,
	NULL,
};

static u64 cputime_read_counter(struct perf_event *event)
{
	int cpu = event->oncpu;

	return kcpustat_cpu(cpu).cpustat[event->hw.config];
}

static void perf_cputime_update(struct perf_event *event)
{
	u64 prev, now;
	s64 delta;

	/* Careful, an NMI might modify the previous event value: */
again:
	prev = local64_read(&event->hw.prev_count);
	now = cputime_read_counter(event);

	if (local64_cmpxchg(&event->hw.prev_count, prev, now) != prev)
		goto again;

	delta = now - prev;
	local64_add(delta, &event->count);
}

static void cputime_event_start(struct perf_event *event, int flags)
{
	u64 now = cputime_read_counter(event);

	local64_set(&event->hw.prev_count, now);
}

static void cputime_event_stop(struct perf_event *event, int flags)
{
	perf_cputime_update(event);
}

static int cputime_event_add(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_START)
		cputime_event_start(event, flags);

	return 0;
}

static void cputime_event_del(struct perf_event *event, int flags)
{
	cputime_event_stop(event, PERF_EF_UPDATE);
}

static void perf_cputime_read(struct perf_event *event)
{
	perf_cputime_update(event);
}

static int cputime_event_init(struct perf_event *event)
{
	u64 cfg = event->attr.config;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/* unsupported modes and filters */
	if (event->attr.exclude_user   ||
	    event->attr.exclude_kernel ||
	    event->attr.exclude_hv     ||
	    event->attr.exclude_idle   ||
	    event->attr.exclude_host   ||
	    event->attr.exclude_guest  ||
	    event->attr.sample_period) /* no sampling */
		return -EINVAL;

	if (cfg >= PERF_CPUTIME_MAX)
		return -EINVAL;

	event->hw.config = map[cfg];
	return 0;
}

static struct pmu perf_cputime = {
	.task_ctx_nr	= perf_sw_context,
	.attr_groups	= cputime_attr_groups,
	.capabilities	= PERF_PMU_CAP_NO_INTERRUPT,
	.event_init	= cputime_event_init,
	.add		= cputime_event_add,
	.del		= cputime_event_del,
	.start		= cputime_event_start,
	.stop		= cputime_event_stop,
	.read		= perf_cputime_read,
};

int __init perf_cputime_register(void)
{
	return perf_pmu_register(&perf_cputime, "cputime", -1);
}
