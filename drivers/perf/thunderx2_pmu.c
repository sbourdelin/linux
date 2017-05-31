/*
 * CAVIUM THUNDERX2 SoC PMU UNCORE
 *
 * Copyright (C) 2017 Cavium Inc.
 * Author: Ganapatrao Kulkarni <gkulkarni@cavium.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/acpi.h>
#include <linux/arm-smccc.h>
#include <linux/perf_event.h>
#include <linux/platform_device.h>

#define UNCORE_MAX_COUNTERS		4
#define UNCORE_L3_MAX_TILES		16
#define UNCORE_DMC_MAX_CHANNELS		8

#define UNCORE_HRTIMER_INTERVAL		(2 * NSEC_PER_SEC)
#define GET_EVENTID(ev)			((ev->hw.config) & 0x1ff)
#define GET_COUNTERID(ev)		((ev->hw.idx) & 0xf)
#define GET_CHANNELID(pmu_uncore)	(pmu_uncore->channel)

#define DMC_COUNTER_CTL			0x234
#define DMC_COUNTER_DATA		0x240
#define L3C_COUNTER_CTL			0xA8
#define L3C_COUNTER_DATA		0xAC

#define SELECT_CHANNEL			0xC
#define THUNDERX2_SMC_ID		0xC200FF00
#define THUNDERX2_SMC_READ		0xB004
#define THUNDERX2_SMC_WRITE		0xB005

enum thunderx2_uncore_l3_events {
	L3_EVENT_NBU_CANCEL = 1,
	L3_EVENT_DIB_RETRY,
	L3_EVENT_DOB_RETRY,
	L3_EVENT_DIB_CREDIT_RETRY,
	L3_EVENT_DOB_CREDIT_RETRY,
	L3_EVENT_FORCE_RETRY,
	L3_EVENT_IDX_CONFLICT_RETRY,
	L3_EVENT_EVICT_CONFLICT_RETRY,
	L3_EVENT_BANK_CONFLICT_RETRY,
	L3_EVENT_FILL_ENTRY_RETRY,
	L3_EVENT_EVICT_NOT_READY_RETRY,
	L3_EVENT_L3_RETRY,
	L3_EVENT_READ_REQ,
	L3_EVENT_WRITE_BACK_REQ,
	L3_EVENT_INVALIDATE_NWRITE_REQ,
	L3_EVENT_INV_REQ,
	L3_EVENT_SELF_REQ,
	L3_EVENT_REQ,
	L3_EVENT_EVICT_REQ,
	L3_EVENT_INVALIDATE_NWRITE_HIT,
	L3_EVENT_INVALIDATE_HIT,
	L3_EVENT_SELF_HIT,
	L3_EVENT_READ_HIT,
	L3_EVENT_MAX,
};

enum thunderx2_uncore_dmc_events {
	DMC_EVENT_COUNT_CYCLES = 1,
	DMC_EVENT_RES2,
	DMC_EVENT_RES3,
	DMC_EVENT_RES4,
	DMC_EVENT_RES5,
	DMC_EVENT_RES6,
	DMC_EVENT_RES7,
	DMC_EVENT_RES8,
	DMC_EVENT_READ_64B,
	DMC_EVENT_READ_LESS_THAN_64B,
	DMC_EVENT_WRITE,
	DMC_EVENT_TXN_CYCLES,
	DMC_EVENT_DATA_TXFERED,
	DMC_EVENT_CANCELLED_READ_TXN,
	DMC_EVENT_READ_TXN_CONSUMED,
	DMC_EVENT_MAX,
};

enum thunderx2_uncore_type {
	PMU_TYPE_INVALID,
	PMU_TYPE_L3C,
	PMU_TYPE_DMC,
};

struct active_timer {
	struct perf_event *event;
	struct hrtimer hrtimer;
};

/*
 * pmu on each socket has 2 uncore devices(dmc and l3),
 * each uncore device has up to 16 channels, each channel can sample
 * events independently with counters up to 4.
 *
 * struct thunderx2_pmu_uncore_channel created per channel.
 * struct thunderx2_pmu_uncore_dev per uncore device.
 * struct thunderx2_pmu created per socket.
 */
struct thunderx2_pmu_uncore_channel {
	struct thunderx2_pmu_uncore_dev *uncore_dev;
	struct pmu pmu;
	int counter;
	int channel;
	DECLARE_BITMAP(counter_mask, UNCORE_MAX_COUNTERS);
	struct active_timer *active_timers;
	/* to sync counter alloc/release */
	raw_spinlock_t lock;
};

struct thunderx2_pmu_uncore_dev {
	char *name;
	enum thunderx2_uncore_type type;
	unsigned long base;
	struct thunderx2_pmu *thunderx2_pmu;
	int node;
	struct cpumask cpu_mask;
	u32    max_counters;
	u32    max_channels;
	u32    max_events;
	u64 hrtimer_interval;
	/* this lock synchronizes across channels */
	raw_spinlock_t lock;
	const struct attribute_group **attr_groups;
	void	(*init_cntr_base)(struct perf_event *event,
			struct thunderx2_pmu_uncore_dev *uncore_dev);
	void	(*select_channel)(struct perf_event *event);
	void	(*stop_event)(struct perf_event *event);
	void	(*start_event)(struct perf_event *event, int flags);
};

struct thunderx2_pmu {
	struct device *dev;
	unsigned long base_pa;
};

static inline struct thunderx2_pmu_uncore_channel *
pmu_to_thunderx2_pmu_uncore(struct pmu *pmu)
{
	return container_of(pmu, struct thunderx2_pmu_uncore_channel, pmu);
}

/*
 * sysfs format attributes
 */
static ssize_t thunderx2_pmu_format_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *eattr;

	eattr = container_of(attr, struct dev_ext_attribute, attr);
	return sprintf(buf, "%s\n", (char *) eattr->var);
}

#define FORMAT_ATTR(_name, _config) \
	(&((struct dev_ext_attribute[]) { \
	   { \
	   .attr = __ATTR(_name, 0444, thunderx2_pmu_format_show, NULL), \
	   .var = (void *) _config, \
	   } \
	})[0].attr.attr)

static struct attribute *l3c_pmu_format_attrs[] = {
	FORMAT_ATTR(event,	"config:0-4"),
	NULL,
};

static struct attribute *dmc_pmu_format_attrs[] = {
	FORMAT_ATTR(event,	"config:0-4"),
	NULL,
};

static const struct attribute_group l3c_pmu_format_attr_group = {
	.name = "format",
	.attrs = l3c_pmu_format_attrs,
};

static const struct attribute_group dmc_pmu_format_attr_group = {
	.name = "format",
	.attrs = dmc_pmu_format_attrs,
};

/*
 * sysfs event attributes
 */
static ssize_t thunderx2_pmu_event_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *eattr;

	eattr = container_of(attr, struct dev_ext_attribute, attr);
	return sprintf(buf, "config=0x%lx\n", (unsigned long) eattr->var);
}

#define EVENT_ATTR(_name, _config) \
	(&((struct dev_ext_attribute[]) { \
	   { \
	   .attr = __ATTR(_name, 0444, thunderx2_pmu_event_show, NULL), \
	   .var = (void *) _config, \
	   } \
	 })[0].attr.attr)

static struct attribute *l3c_pmu_events_attrs[] = {
	EVENT_ATTR(nbu_cancel,			L3_EVENT_NBU_CANCEL),
	EVENT_ATTR(dib_retry,			L3_EVENT_DIB_RETRY),
	EVENT_ATTR(dob_retry,			L3_EVENT_DOB_RETRY),
	EVENT_ATTR(dib_credit_retry,		L3_EVENT_DIB_CREDIT_RETRY),
	EVENT_ATTR(dob_credit_retry,		L3_EVENT_DOB_CREDIT_RETRY),
	EVENT_ATTR(force_retry,			L3_EVENT_FORCE_RETRY),
	EVENT_ATTR(idx_conflict_retry,		L3_EVENT_IDX_CONFLICT_RETRY),
	EVENT_ATTR(evict_conflict_retry,	L3_EVENT_EVICT_CONFLICT_RETRY),
	EVENT_ATTR(bank_conflict_retry,		L3_EVENT_BANK_CONFLICT_RETRY),
	EVENT_ATTR(fill_entry_retry,		L3_EVENT_FILL_ENTRY_RETRY),
	EVENT_ATTR(evict_not_ready_retry,	L3_EVENT_EVICT_NOT_READY_RETRY),
	EVENT_ATTR(l3_retry,			L3_EVENT_L3_RETRY),
	EVENT_ATTR(read_requests,		L3_EVENT_READ_REQ),
	EVENT_ATTR(write_back_requests,		L3_EVENT_WRITE_BACK_REQ),
	EVENT_ATTR(inv_nwrite_requests,		L3_EVENT_INVALIDATE_NWRITE_REQ),
	EVENT_ATTR(inv_requests,		L3_EVENT_INV_REQ),
	EVENT_ATTR(self_requests,		L3_EVENT_SELF_REQ),
	EVENT_ATTR(requests,			L3_EVENT_REQ),
	EVENT_ATTR(evict_requests,		L3_EVENT_EVICT_REQ),
	EVENT_ATTR(inv_nwrite_hit,		L3_EVENT_INVALIDATE_NWRITE_HIT),
	EVENT_ATTR(inv_hit,			L3_EVENT_INVALIDATE_HIT),
	EVENT_ATTR(self_hit,			L3_EVENT_SELF_HIT),
	EVENT_ATTR(read_hit,			L3_EVENT_READ_HIT),
	NULL,
};

static struct attribute *dmc_pmu_events_attrs[] = {
	EVENT_ATTR(cnt_cycles,			DMC_EVENT_COUNT_CYCLES),
	EVENT_ATTR(read_64b_txns,		DMC_EVENT_READ_64B),
	EVENT_ATTR(read_less_than_64b_txns,	DMC_EVENT_READ_LESS_THAN_64B),
	EVENT_ATTR(write_txns,			DMC_EVENT_WRITE),
	EVENT_ATTR(txn_cycles,			DMC_EVENT_TXN_CYCLES),
	EVENT_ATTR(data_txfered,		DMC_EVENT_DATA_TXFERED),
	EVENT_ATTR(cancelled_read_txn,		DMC_EVENT_CANCELLED_READ_TXN),
	EVENT_ATTR(read_txn_consumed,		DMC_EVENT_READ_TXN_CONSUMED),
	NULL,
};

static const struct attribute_group l3c_pmu_events_attr_group = {
	.name = "events",
	.attrs = l3c_pmu_events_attrs,
};

static const struct attribute_group dmc_pmu_events_attr_group = {
	.name = "events",
	.attrs = dmc_pmu_events_attrs,
};

/*
 * sysfs cpumask attributes
 */
static ssize_t thunderx2_pmu_cpumask_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct thunderx2_pmu_uncore_channel *pmu_uncore =
		pmu_to_thunderx2_pmu_uncore(dev_get_drvdata(dev));
	return cpumap_print_to_pagebuf(true, buf,
			&pmu_uncore->uncore_dev->cpu_mask);
}

static DEVICE_ATTR(cpumask, 0444, thunderx2_pmu_cpumask_show, NULL);

static struct attribute *thunderx2_pmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static const struct attribute_group pmu_cpumask_attr_group = {
	.attrs = thunderx2_pmu_cpumask_attrs,
};

/*
 * Per PMU device attribute groups
 */
static const struct attribute_group *l3c_pmu_attr_groups[] = {
	&l3c_pmu_format_attr_group,
	&pmu_cpumask_attr_group,
	&l3c_pmu_events_attr_group,
	NULL
};

static const struct attribute_group *dmc_pmu_attr_groups[] = {
	&dmc_pmu_format_attr_group,
	&pmu_cpumask_attr_group,
	&dmc_pmu_events_attr_group,
	NULL
};

static inline struct active_timer *get_active_timer(struct hrtimer *hrt)
{
	return container_of(hrt, struct active_timer, hrtimer);
}

static inline u32 reg_readl(unsigned long addr)
{
	return readl((void __iomem *)addr);
}

static inline void reg_writel(u32 val, unsigned long addr)
{
	writel(val, (void __iomem *)addr);
}

static int alloc_counter(struct thunderx2_pmu_uncore_channel *pmu_uncore)
{
	int counter;

	raw_spin_lock(&pmu_uncore->lock);
	counter = find_first_zero_bit(pmu_uncore->counter_mask,
				pmu_uncore->uncore_dev->max_counters);
	if (counter == pmu_uncore->uncore_dev->max_counters) {
		raw_spin_unlock(&pmu_uncore->lock);
		return -ENOSPC;
	}
	set_bit(counter, pmu_uncore->counter_mask);
	raw_spin_unlock(&pmu_uncore->lock);
	return counter;
}

static void free_counter(struct thunderx2_pmu_uncore_channel *pmu_uncore,
					int counter)
{
	raw_spin_lock(&pmu_uncore->lock);
	clear_bit(counter, pmu_uncore->counter_mask);
	raw_spin_unlock(&pmu_uncore->lock);
}

static void secure_write_reg(uint32_t value, uint64_t pa)
{
	struct arm_smccc_res res;

	arm_smccc_smc(THUNDERX2_SMC_ID, THUNDERX2_SMC_WRITE,
			0, pa, value, 0, 0, 0, &res);
}

static uint32_t secure_read_reg(uint64_t pa)
{
	struct arm_smccc_res res;

	arm_smccc_smc(THUNDERX2_SMC_ID, THUNDERX2_SMC_READ,
			0, pa, 0, 0, 0, 0,  &res);
	return res.a0;
}

/*
 * DMC and L3 counter interface is muxed across all channels.
 * hence we need to select the channel before accessing counter
 * data/control registers.
 *
 *  L3 tile/DMC channel selection is through secure register
 */
static void uncore_select_channel_l3c(struct perf_event *event)
{
	u32 val;
	struct thunderx2_pmu_uncore_channel *pmu_uncore;
	unsigned long pa;

	pmu_uncore = pmu_to_thunderx2_pmu_uncore(event->pmu);
	pa = pmu_uncore->uncore_dev->thunderx2_pmu->base_pa + SELECT_CHANNEL;

	val = secure_read_reg(pa);
	/* bits [03:00] selects L3C tile */
	val &= ~(0xf);
	val |= GET_CHANNELID(pmu_uncore);
	secure_write_reg(val, pa);
}

static void uncore_select_channel_dmc(struct perf_event *event)
{
	u32 val;
	struct thunderx2_pmu_uncore_channel *pmu_uncore;
	unsigned long pa;

	pmu_uncore = pmu_to_thunderx2_pmu_uncore(event->pmu);
	pa = pmu_uncore->uncore_dev->thunderx2_pmu->base_pa + SELECT_CHANNEL;

	val = secure_read_reg(pa);
	/* bits [06:04] selects DMC channel */
	val &= ~(0x7 << 4);
	val |= (GET_CHANNELID(pmu_uncore) << 4);
	secure_write_reg(val, pa);
}

static void uncore_start_event_l3c(struct perf_event *event, int flags)
{
	u32 val;
	struct hw_perf_event *hwc = &event->hw;

	/* event id encoded in bits [07:03] */
	val = GET_EVENTID(event) << 3;
	reg_writel(val, hwc->config_base);

	if (flags & PERF_EF_RELOAD) {
		u64 prev_raw_count =
			local64_read(&event->hw.prev_count);
		reg_writel(prev_raw_count, hwc->event_base);
	}
	local64_set(&event->hw.prev_count,
			reg_readl(hwc->event_base));
}

static void uncore_start_event_dmc(struct perf_event *event, int flags)
{
	u32 val, event_shift = 8;
	struct hw_perf_event *hwc = &event->hw;

	/* enable and start counters and read current value in prev_count */
	val = reg_readl(hwc->config_base);

	/* 8 bits for each counter,
	 * bits [05:01] of a counter to set event type.
	 */
	reg_writel((val & ~(0x1f << (((GET_COUNTERID(event)) *
		event_shift) + 1))) |
		(GET_EVENTID(event) <<
		 (((GET_COUNTERID(event)) * event_shift) + 1)),
		hwc->config_base);

	if (flags & PERF_EF_RELOAD) {
		u64 prev_raw_count =
			local64_read(&event->hw.prev_count);
		reg_writel(prev_raw_count, hwc->event_base);
	}
	local64_set(&event->hw.prev_count,
			reg_readl(hwc->event_base));
}

static void uncore_stop_event_l3c(struct perf_event *event)
{
	reg_writel(0, event->hw.config_base);
}

static void uncore_stop_event_dmc(struct perf_event *event)
{
	u32 val, event_shift = 8;
	struct hw_perf_event *hwc = &event->hw;

	val = reg_readl(hwc->config_base);
	reg_writel((val & ~(0xff << ((GET_COUNTERID(event)) * event_shift))),
			hwc->config_base);
}

static void init_cntr_base_l3c(struct perf_event *event,
		struct thunderx2_pmu_uncore_dev *uncore_dev) {

	struct hw_perf_event *hwc = &event->hw;

	/* counter ctrl/data reg offset at 8 */
	hwc->config_base = uncore_dev->base
		+ L3C_COUNTER_CTL + (8 * GET_COUNTERID(event));
	hwc->event_base =  uncore_dev->base
		+ L3C_COUNTER_DATA + (8 * GET_COUNTERID(event));
}

static void init_cntr_base_dmc(struct perf_event *event,
		struct thunderx2_pmu_uncore_dev *uncore_dev) {

	struct hw_perf_event *hwc = &event->hw;

	hwc->config_base = uncore_dev->base
		+ DMC_COUNTER_CTL;
	/* counter data reg offset at 0xc */
	hwc->event_base =  uncore_dev->base
		+ DMC_COUNTER_DATA + (0xc * GET_COUNTERID(event));
}

static void thunderx2_uncore_update(struct perf_event *event)
{
	s64 prev, new = 0;
	u64 delta;
	struct hw_perf_event *hwc = &event->hw;
	struct thunderx2_pmu_uncore_channel *pmu_uncore;
	enum thunderx2_uncore_type type;

	pmu_uncore = pmu_to_thunderx2_pmu_uncore(event->pmu);
	type = pmu_uncore->uncore_dev->type;

	if (pmu_uncore->uncore_dev->select_channel)
		pmu_uncore->uncore_dev->select_channel(event);

	new = reg_readl(hwc->event_base);
	prev = local64_xchg(&hwc->prev_count, new);

	/* handle rollover of counters */
	if (new < prev)
		delta = (((1UL << 32) - prev) + new);
	else
		delta = new - prev;

	local64_add(delta, &event->count);
}

enum thunderx2_uncore_type get_uncore_device_type(struct acpi_device *adev)
{
	int i = 0;
	struct acpi_uncore_device {
		__u8 id[ACPI_ID_LEN];
		enum thunderx2_uncore_type type;
	} devices[] = {
		{"CAV901D", PMU_TYPE_L3C},
		{"CAV901F", PMU_TYPE_DMC},
		{"", PMU_TYPE_INVALID},
	};

	while (devices[i].type != PMU_TYPE_INVALID) {
		if (!strcmp(acpi_device_hid(adev), devices[i].id))
			return devices[i].type;
		i++;
	}
	return PMU_TYPE_INVALID;
}

static int thunderx2_uncore_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct thunderx2_pmu_uncore_channel *pmu_uncore;
	struct perf_event *sibling;

	/* Test the event attr type check for PMU enumeration */
	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/*
	 * SOC PMU counters are shared across all cores.
	 * Therefore, it does not support per-process mode.
	 * Also, it does not support event sampling mode.
	 */
	if (is_sampling_event(event) || event->attach_state & PERF_ATTACH_TASK)
		return -EINVAL;

	/* SOC counters do not have usr/os/guest/host bits */
	if (event->attr.exclude_user || event->attr.exclude_kernel ||
	    event->attr.exclude_host || event->attr.exclude_guest)
		return -EINVAL;

	if (event->cpu < 0)
		return -EINVAL;

	pmu_uncore = pmu_to_thunderx2_pmu_uncore(event->pmu);

	if (!pmu_uncore)
		return -ENODEV;

	/* Pick one core from the node to use for cpumask attributes */
	event->cpu = cpumask_first(
			cpumask_of_node(pmu_uncore->uncore_dev->node));

	if (event->cpu >= nr_cpu_ids)
		return -EINVAL;

	if (event->attr.config >= pmu_uncore->uncore_dev->max_events)
		return -EINVAL;

	/* store event id */
	hwc->config = event->attr.config;

	/*
	 * We must NOT create groups containing mixed PMUs,
	 * although software events are acceptable
	 */
	if (event->group_leader->pmu != event->pmu &&
			!is_software_event(event->group_leader))
		return -EINVAL;

	list_for_each_entry(sibling, &event->group_leader->sibling_list,
			group_entry)
		if (sibling->pmu != event->pmu &&
				!is_software_event(sibling))
			return -EINVAL;

	return 0;
}

static void thunderx2_uncore_start(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	struct thunderx2_pmu_uncore_channel *pmu_uncore;
	struct thunderx2_pmu_uncore_dev *uncore_dev;
	unsigned long irqflags;
	struct active_timer *active_timer;

	hwc->state = 0;
	pmu_uncore = pmu_to_thunderx2_pmu_uncore(event->pmu);
	uncore_dev = pmu_uncore->uncore_dev;

	raw_spin_lock_irqsave(&uncore_dev->lock, irqflags);

	if (uncore_dev->select_channel)
		uncore_dev->select_channel(event);
	uncore_dev->start_event(event, flags);
	raw_spin_unlock_irqrestore(&uncore_dev->lock, irqflags);

	perf_event_update_userpage(event);
	active_timer = &pmu_uncore->active_timers[GET_COUNTERID(event)];
	active_timer->event = event;

	if (!hrtimer_active(&active_timer->hrtimer))
		hrtimer_start(&active_timer->hrtimer,
			ns_to_ktime(uncore_dev->hrtimer_interval),
			HRTIMER_MODE_REL_PINNED);
}

static void thunderx2_uncore_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	struct thunderx2_pmu_uncore_channel *pmu_uncore;
	struct thunderx2_pmu_uncore_dev *uncore_dev;
	unsigned long irqflags;

	if (hwc->state & PERF_HES_UPTODATE)
		return;

	pmu_uncore = pmu_to_thunderx2_pmu_uncore(event->pmu);
	uncore_dev = pmu_uncore->uncore_dev;

	raw_spin_lock_irqsave(&uncore_dev->lock, irqflags);

	if (uncore_dev->select_channel)
		uncore_dev->select_channel(event);
	uncore_dev->stop_event(event);

	WARN_ON_ONCE(hwc->state & PERF_HES_STOPPED);
	hwc->state |= PERF_HES_STOPPED;
	if ((flags & PERF_EF_UPDATE) && !(hwc->state & PERF_HES_UPTODATE)) {
		thunderx2_uncore_update(event);
		hwc->state |= PERF_HES_UPTODATE;
	}
	raw_spin_unlock_irqrestore(&uncore_dev->lock, irqflags);
}

static int thunderx2_uncore_add(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	struct thunderx2_pmu_uncore_channel *pmu_uncore;
	struct thunderx2_pmu_uncore_dev *uncore_dev;

	pmu_uncore = pmu_to_thunderx2_pmu_uncore(event->pmu);
	uncore_dev = pmu_uncore->uncore_dev;

	/* Allocate a free counter */
	hwc->idx  = alloc_counter(pmu_uncore);
	if (hwc->idx < 0)
		return -EAGAIN;

	/* set counter control and data registers base address */
	uncore_dev->init_cntr_base(event, uncore_dev);

	hwc->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;
	if (flags & PERF_EF_START)
		thunderx2_uncore_start(event, PERF_EF_RELOAD);

	return 0;
}

static void thunderx2_uncore_del(struct perf_event *event, int flags)
{
	struct thunderx2_pmu_uncore_channel *pmu_uncore =
			pmu_to_thunderx2_pmu_uncore(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	thunderx2_uncore_stop(event, PERF_EF_UPDATE);

	hrtimer_cancel(
		&pmu_uncore->active_timers[GET_COUNTERID(event)].hrtimer);
	pmu_uncore->active_timers[GET_COUNTERID(event)].event = NULL;

	/* clear the assigned counter */
	free_counter(pmu_uncore, GET_COUNTERID(event));

	perf_event_update_userpage(event);
	hwc->idx = -1;
}

static void thunderx2_uncore_read(struct perf_event *event)
{
	unsigned long irqflags;
	struct thunderx2_pmu_uncore_channel *pmu_uncore =
			pmu_to_thunderx2_pmu_uncore(event->pmu);

	raw_spin_lock_irqsave(&pmu_uncore->uncore_dev->lock, irqflags);
	thunderx2_uncore_update(event);
	raw_spin_unlock_irqrestore(&pmu_uncore->uncore_dev->lock, irqflags);
}

static enum hrtimer_restart thunderx2_uncore_hrtimer_callback(
		struct hrtimer *hrt)
{
	struct thunderx2_pmu_uncore_channel *pmu_uncore;
	struct perf_event *event;
	unsigned long irqflags;
	struct active_timer *active_timer;

	active_timer = get_active_timer(hrt);
	event = active_timer->event;

	pmu_uncore = pmu_to_thunderx2_pmu_uncore(event->pmu);

	raw_spin_lock_irqsave(&pmu_uncore->uncore_dev->lock, irqflags);
	thunderx2_uncore_update(event);
	raw_spin_unlock_irqrestore(&pmu_uncore->uncore_dev->lock, irqflags);

	hrtimer_forward_now(hrt,
			ns_to_ktime(
				pmu_uncore->uncore_dev->hrtimer_interval));
	return HRTIMER_RESTART;
}

static int thunderx2_pmu_uncore_register(
		struct thunderx2_pmu_uncore_channel *pmu_uncore)
{
	struct device *dev = pmu_uncore->uncore_dev->thunderx2_pmu->dev;
	char *name = pmu_uncore->uncore_dev->name;
	int channel = pmu_uncore->channel;

	/* Perf event registration */
	pmu_uncore->pmu = (struct pmu) {
		.attr_groups	= pmu_uncore->uncore_dev->attr_groups,
		.task_ctx_nr	= perf_invalid_context,
		.event_init	= thunderx2_uncore_event_init,
		.add		= thunderx2_uncore_add,
		.del		= thunderx2_uncore_del,
		.start		= thunderx2_uncore_start,
		.stop		= thunderx2_uncore_stop,
		.read		= thunderx2_uncore_read,
	};

	pmu_uncore->pmu.name = devm_kasprintf(dev, GFP_KERNEL,
			"%s_%d", name, channel);

	return perf_pmu_register(&pmu_uncore->pmu, pmu_uncore->pmu.name, -1);
}

static int thunderx2_pmu_uncore_add(struct thunderx2_pmu *thunderx2_pmu,
		struct thunderx2_pmu_uncore_dev *uncore_dev,
		int channel)
{
	struct device *dev = thunderx2_pmu->dev;
	struct thunderx2_pmu_uncore_channel *pmu_uncore;
	int ret;
	int counter;

	pmu_uncore = devm_kzalloc(dev, sizeof(*pmu_uncore), GFP_KERNEL);
	if (!pmu_uncore)
		return -ENOMEM;

	pmu_uncore->uncore_dev = uncore_dev;
	pmu_uncore->channel = channel;

	/* we can run up to (max_counters * max_channels) events simultaneously.
	 * allocate hrtimers per channel.
	 */
	pmu_uncore->active_timers = devm_kzalloc(dev,
			sizeof(struct active_timer) * uncore_dev->max_counters,
			GFP_KERNEL);

	for (counter = 0; counter < uncore_dev->max_counters; counter++) {
		hrtimer_init(&pmu_uncore->active_timers[counter].hrtimer,
				CLOCK_MONOTONIC,
				HRTIMER_MODE_REL);
		pmu_uncore->active_timers[counter].hrtimer.function =
				thunderx2_uncore_hrtimer_callback;
	}

	ret = thunderx2_pmu_uncore_register(pmu_uncore);
	if (ret) {
		dev_err(dev, "%s PMU: Failed to init perf driver\n",
				uncore_dev->name);
		return -ENODEV;
	}

	dev_dbg(dev, "%s PMU UNCORE registered\n", pmu_uncore->pmu.name);
	return ret;
}

static struct thunderx2_pmu_uncore_dev *init_pmu_uncore_dev(
		struct thunderx2_pmu *thunderx2_pmu,
		acpi_handle handle, struct acpi_device *adev, u32 type)
{
	struct device *dev = thunderx2_pmu->dev;
	struct thunderx2_pmu_uncore_dev *uncore_dev;
	unsigned long base;
	struct resource res;
	struct resource_entry *rentry;
	struct list_head list;
	int ret;

	INIT_LIST_HEAD(&list);
	ret = acpi_dev_get_resources(adev, &list, NULL, NULL);
	if (ret <= 0) {
		dev_err(dev, "failed to parse _CRS method, error %d\n", ret);
		return NULL;
	}

	list_for_each_entry(rentry, &list, node) {
		if (resource_type(rentry->res) == IORESOURCE_MEM) {
			res = *rentry->res;
			break;
		}
	}

	if (!rentry->res)
		return NULL;

	acpi_dev_free_resource_list(&list);

	base = (unsigned long)devm_ioremap_resource(dev, &res);
	if (IS_ERR((void *)base)) {
		dev_err(dev, "PMU type %d: Fail to map resource\n", type);
		return NULL;
	}

	uncore_dev = devm_kzalloc(dev, sizeof(*uncore_dev), GFP_KERNEL);
	if (!uncore_dev)
		return NULL;

	uncore_dev->thunderx2_pmu = thunderx2_pmu;
	uncore_dev->type = type;
	uncore_dev->base = base;
	uncore_dev->node = dev_to_node(dev);

	/* Pick one core from the node to use for cpumask attributes */
	cpumask_set_cpu(cpumask_first(cpumask_of_node(uncore_dev->node)),
			&uncore_dev->cpu_mask);
	raw_spin_lock_init(&uncore_dev->lock);

	switch (uncore_dev->type) {
	case PMU_TYPE_L3C:
		uncore_dev->max_counters = UNCORE_MAX_COUNTERS;
		uncore_dev->max_channels = UNCORE_L3_MAX_TILES;
		uncore_dev->max_events = L3_EVENT_MAX;
		uncore_dev->hrtimer_interval = UNCORE_HRTIMER_INTERVAL;
		uncore_dev->attr_groups = l3c_pmu_attr_groups;
		uncore_dev->name = devm_kasprintf(dev, GFP_KERNEL,
				"uncore_l3c_%d", uncore_dev->node);
		uncore_dev->init_cntr_base = init_cntr_base_l3c;
		uncore_dev->select_channel = uncore_select_channel_l3c;
		uncore_dev->start_event = uncore_start_event_l3c;
		uncore_dev->stop_event = uncore_stop_event_l3c;
		break;
	case PMU_TYPE_DMC:
		uncore_dev->max_counters = UNCORE_MAX_COUNTERS;
		uncore_dev->max_channels = UNCORE_DMC_MAX_CHANNELS;
		uncore_dev->max_events = DMC_EVENT_MAX;
		uncore_dev->hrtimer_interval = UNCORE_HRTIMER_INTERVAL;
		uncore_dev->attr_groups = dmc_pmu_attr_groups;
		uncore_dev->name = devm_kasprintf(dev, GFP_KERNEL,
				"uncore_dmc_%d", uncore_dev->node);
		uncore_dev->init_cntr_base = init_cntr_base_dmc;
		uncore_dev->select_channel = uncore_select_channel_dmc;
		uncore_dev->start_event = uncore_start_event_dmc;
		uncore_dev->stop_event = uncore_stop_event_dmc;
		break;
	case PMU_TYPE_INVALID:
		uncore_dev = NULL;
		break;
	}

	return uncore_dev;
}

static acpi_status thunderx2_pmu_uncore_dev_add(acpi_handle handle, u32 level,
				    void *data, void **return_value)
{
	struct thunderx2_pmu *thunderx2_pmu = data;
	struct thunderx2_pmu_uncore_dev *uncore_dev;
	struct acpi_device *adev;
	enum thunderx2_uncore_type type;
	int channel;

	if (acpi_bus_get_device(handle, &adev))
		return AE_OK;
	if (acpi_bus_get_status(adev) || !adev->status.present)
		return AE_OK;

	type = get_uncore_device_type(adev);
	if (type == PMU_TYPE_INVALID)
		return AE_OK;

	uncore_dev = init_pmu_uncore_dev(thunderx2_pmu, handle, adev, type);

	if (!uncore_dev)
		return AE_ERROR;

	for (channel = 0; channel < uncore_dev->max_channels; channel++) {
		if (thunderx2_pmu_uncore_add(thunderx2_pmu, uncore_dev,
					channel)) {
			/* Can't add the PMU device, abort */
			return AE_ERROR;
		}
	}
	return AE_OK;
}

static int thunderx2_uncore_dev_probe(struct thunderx2_pmu *thunderx2_pmu,
				   struct platform_device *pdev)
{
	struct device *dev = thunderx2_pmu->dev;
	acpi_handle handle;
	acpi_status status;

	if (!has_acpi_companion(&pdev->dev))
		return -ENODEV;

	handle = ACPI_HANDLE(dev);
	if (!handle)
		return -EINVAL;

	status = acpi_walk_namespace(ACPI_TYPE_DEVICE, handle, 1,
				     thunderx2_pmu_uncore_dev_add,
				     NULL, thunderx2_pmu, NULL);
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "failed to probe PMU devices\n");
		return_ACPI_STATUS(status);
	}

	dev_info(dev, "node%d: pmu uncore registered\n", dev_to_node(dev));
	return 0;
}

static const struct acpi_device_id thunderx2_uncore_acpi_match[] = {
	{"CAV901C", 0},
	{},
};
MODULE_DEVICE_TABLE(acpi, thunderx2_uncore_acpi_match);

static int thunderx2_uncore_probe(struct platform_device *pdev)
{
	struct thunderx2_pmu *thunderx2_pmu;
	struct resource *res;
	struct device *dev = &pdev->dev;

	set_dev_node(dev, acpi_get_node(ACPI_HANDLE(dev)));
	thunderx2_pmu = devm_kzalloc(dev, sizeof(*thunderx2_pmu), GFP_KERNEL);
	if (!thunderx2_pmu)
		return -ENOMEM;

	thunderx2_pmu->dev = dev;
	platform_set_drvdata(pdev, thunderx2_pmu);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	thunderx2_pmu->base_pa = res->start;

	/* Walk through the tree for all PMU UNCORE devices */
	return thunderx2_uncore_dev_probe(thunderx2_pmu, pdev);
}

static struct platform_driver thunderx2_uncore_driver = {
	.probe = thunderx2_uncore_probe,
	.driver = {
		.name		= "thunderx2-uncore-pmu",
		.acpi_match_table = ACPI_PTR(thunderx2_uncore_acpi_match),
	},
};

builtin_platform_driver(thunderx2_uncore_driver);
