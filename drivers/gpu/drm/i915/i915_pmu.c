#include <linux/perf_event.h>
#include <linux/pm_runtime.h>

#include "i915_drv.h"
#include "intel_ringbuffer.h"

#define FREQUENCY 200
#define PERIOD max_t(u64, 10000, NSEC_PER_SEC / FREQUENCY)

#define RING_MASK 0xffffffff
#define RING_MAX 32

#define ENGINE_SAMPLE_MASK (0xf)
#define ENGINE_SAMPLE_BITS (4)

static const unsigned int engine_map[I915_NUM_ENGINES] = {
	[RCS] = I915_SAMPLE_RCS,
	[BCS] = I915_SAMPLE_BCS,
	[VCS] = I915_SAMPLE_VCS,
	[VCS2] = I915_SAMPLE_VCS2,
	[VECS] = I915_SAMPLE_VECS,
};

static const unsigned int user_engine_map[I915_NUM_ENGINES] = {
	[I915_SAMPLE_RCS] = RCS,
	[I915_SAMPLE_BCS] = BCS,
	[I915_SAMPLE_VCS] = VCS,
	[I915_SAMPLE_VCS2] = VCS2,
	[I915_SAMPLE_VECS] = VECS,
};

static bool pmu_needs_timer(struct drm_i915_private *i915, bool gpu_active)
{
	if (gpu_active)
		return i915->pmu.enable;
	else
		return i915->pmu.enable >> 32;
}

void i915_pmu_gt_idle(struct drm_i915_private *i915)
{
	spin_lock_irq(&i915->pmu.lock);
	/*
	 * Signal sampling timer to stop if only engine events are enabled and
	 * GPU went idle.
	 */
	i915->pmu.timer_enabled = pmu_needs_timer(i915, false);
	spin_unlock_irq(&i915->pmu.lock);
}

void i915_pmu_gt_active(struct drm_i915_private *i915)
{
	spin_lock_irq(&i915->pmu.lock);
	/*
	 * Re-enable sampling timer when GPU goes active.
	 */
	if (!i915->pmu.timer_enabled && pmu_needs_timer(i915, true)) {
		hrtimer_start_range_ns(&i915->pmu.timer,
				       ns_to_ktime(PERIOD), 0,
				       HRTIMER_MODE_REL_PINNED);
		i915->pmu.timer_enabled = true;
	}
	spin_unlock_irq(&i915->pmu.lock);
}

static bool grab_forcewake(struct drm_i915_private *i915, bool fw)
{
	if (!fw)
		intel_uncore_forcewake_get(i915, FORCEWAKE_ALL);

	return true;
}

static void engines_sample(struct drm_i915_private *dev_priv)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	bool fw = false;

	if ((dev_priv->pmu.enable & RING_MASK) == 0)
		return;

	if (!dev_priv->gt.awake)
		return;

	if (!intel_runtime_pm_get_if_in_use(dev_priv))
		return;

	for_each_engine(engine, dev_priv, id) {
		unsigned int user_engine = engine_map[id];
		u8 sample_mask;
		u32 val;

		if (WARN_ON_ONCE(id >= ARRAY_SIZE(engine_map)))
			continue;
		else
			user_engine = engine_map[id];

		sample_mask = (dev_priv->pmu.enable >>
			      (ENGINE_SAMPLE_BITS * user_engine)) &
			      ENGINE_SAMPLE_MASK;

		if (!sample_mask)
			continue;

		if (i915_seqno_passed(intel_engine_get_seqno(engine),
				      intel_engine_last_submit(engine)))
			continue;

		if (sample_mask & BIT(I915_SAMPLE_QUEUED))
			engine->pmu_sample[I915_SAMPLE_QUEUED] += PERIOD;

		if (sample_mask & BIT(I915_SAMPLE_BUSY)) {
			fw = grab_forcewake(dev_priv, fw);
			val = I915_READ_FW(RING_MI_MODE(engine->mmio_base));
			if (!(val & MODE_IDLE))
				engine->pmu_sample[I915_SAMPLE_BUSY] += PERIOD;
		}

		if (sample_mask &
		    (BIT(I915_SAMPLE_WAIT) | BIT(I915_SAMPLE_SEMA))) {
			fw = grab_forcewake(dev_priv, fw);
			val = I915_READ_FW(RING_CTL(engine->mmio_base));
			if ((sample_mask & BIT(I915_SAMPLE_WAIT)) &&
			    (val & RING_WAIT))
				engine->pmu_sample[I915_SAMPLE_WAIT] += PERIOD;
			if ((sample_mask & BIT(I915_SAMPLE_SEMA)) &&
			    (val & RING_WAIT_SEMAPHORE))
				engine->pmu_sample[I915_SAMPLE_SEMA] += PERIOD;
		}
	}

	if (fw)
		intel_uncore_forcewake_put(dev_priv, FORCEWAKE_ALL);
	intel_runtime_pm_put(dev_priv);
}

static void frequency_sample(struct drm_i915_private *dev_priv)
{
	if (dev_priv->pmu.enable & BIT_ULL(I915_PMU_ACTUAL_FREQUENCY)) {
		u64 val;

		val = dev_priv->rps.cur_freq;
		if (dev_priv->gt.awake &&
		    intel_runtime_pm_get_if_in_use(dev_priv)) {
			val = I915_READ_NOTRACE(GEN6_RPSTAT1);
			if (INTEL_GEN(dev_priv) >= 9)
				val = (val & GEN9_CAGF_MASK) >> GEN9_CAGF_SHIFT;
			else if (IS_HASWELL(dev_priv) || INTEL_GEN(dev_priv) >= 8)
				val = (val & HSW_CAGF_MASK) >> HSW_CAGF_SHIFT;
			else
				val = (val & GEN6_CAGF_MASK) >> GEN6_CAGF_SHIFT;
			intel_runtime_pm_put(dev_priv);
		}
		val = intel_gpu_freq(dev_priv, val);
		dev_priv->pmu.sample[__I915_SAMPLE_FREQ_ACT] += val * PERIOD;
	}

	if (dev_priv->pmu.enable & BIT_ULL(I915_PMU_REQUESTED_FREQUENCY)) {
		u64 val = intel_gpu_freq(dev_priv, dev_priv->rps.cur_freq);
		dev_priv->pmu.sample[__I915_SAMPLE_FREQ_REQ] += val * PERIOD;
	}
}

static enum hrtimer_restart i915_sample(struct hrtimer *hrtimer)
{
	struct drm_i915_private *i915 =
		container_of(hrtimer, struct drm_i915_private, pmu.timer);

	if (!READ_ONCE(i915->pmu.timer_enabled))
		return HRTIMER_NORESTART;

	engines_sample(i915);
	frequency_sample(i915);

	hrtimer_forward_now(hrtimer, ns_to_ktime(PERIOD));
	return HRTIMER_RESTART;
}

static void i915_pmu_event_destroy(struct perf_event *event)
{
	WARN_ON(event->parent);
}

#define pmu_config_engine(config) ((config) >> 2)
#define pmu_config_sampler(config) ((config) & 3)

static int engine_event_init(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	unsigned int user_engine = pmu_config_engine(event->attr.config);
	unsigned int sample = pmu_config_sampler(event->attr.config);
	enum intel_engine_id engine_id;

	if (WARN_ON_ONCE(user_engine >= ARRAY_SIZE(user_engine_map)))
		return -ENOENT;
	else
		engine_id = user_engine_map[user_engine];

	switch (sample) {
	case I915_SAMPLE_QUEUED:
	case I915_SAMPLE_BUSY:
	case I915_SAMPLE_WAIT:
		break;
	case I915_SAMPLE_SEMA:
		if (INTEL_GEN(i915) < 6)
			return -ENODEV;
		break;
	default:
		return -ENOENT;
	}

	if (!i915->engine[engine_id])
		return -ENODEV;

	return 0;
}

static DEFINE_PER_CPU(struct pt_regs, i915_pmu_pt_regs);

static enum hrtimer_restart hrtimer_sample(struct hrtimer *hrtimer)
{
	struct pt_regs *regs = this_cpu_ptr(&i915_pmu_pt_regs);
	struct perf_sample_data data;
	struct perf_event *event;
	u64 period;

	event = container_of(hrtimer, struct perf_event, hw.hrtimer);
	if (event->state != PERF_EVENT_STATE_ACTIVE)
		return HRTIMER_NORESTART;

	event->pmu->read(event);

	perf_sample_data_init(&data, 0, event->hw.last_period);
	perf_event_overflow(event, &data, regs);

	period = max_t(u64, 10000, event->hw.sample_period);
	hrtimer_forward_now(hrtimer, ns_to_ktime(period));
	return HRTIMER_RESTART;
}

static void init_hrtimer(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;

	if (!is_sampling_event(event))
		return;

	hrtimer_init(&hwc->hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hwc->hrtimer.function = hrtimer_sample;

	if (event->attr.freq) {
		long freq = event->attr.sample_freq;

		event->attr.sample_period = NSEC_PER_SEC / freq;
		hwc->sample_period = event->attr.sample_period;
		local64_set(&hwc->period_left, hwc->sample_period);
		hwc->last_period = hwc->sample_period;
		event->attr.freq = 0;
	}
}

static int i915_pmu_event_init(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	int ret;

	/* XXX ideally only want pid == -1 && cpu == -1 */

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	ret = 0;
	if (event->attr.config < RING_MAX) {
		ret = engine_event_init(event);
	} else switch (event->attr.config) {
	case I915_PMU_ACTUAL_FREQUENCY:
		if (IS_VALLEYVIEW(i915) || IS_CHERRYVIEW(i915))
			ret = -ENODEV; /* requires a mutex for sampling! */
	case I915_PMU_REQUESTED_FREQUENCY:
	case I915_PMU_ENERGY:
	case I915_PMU_RC6_RESIDENCY:
	case I915_PMU_RC6p_RESIDENCY:
	case I915_PMU_RC6pp_RESIDENCY:
		if (INTEL_GEN(i915) < 6)
			ret = -ENODEV;
		break;
	}
	if (ret)
		return ret;

	if (!event->parent)
		event->destroy = i915_pmu_event_destroy;

	init_hrtimer(event);

	return 0;
}

static void i915_pmu_timer_start(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	s64 period;

	if (!is_sampling_event(event))
		return;

	period = local64_read(&hwc->period_left);
	if (period) {
		if (period < 0)
			period = 10000;

		local64_set(&hwc->period_left, 0);
	} else {
		period = max_t(u64, 10000, hwc->sample_period);
	}

	hrtimer_start_range_ns(&hwc->hrtimer,
			       ns_to_ktime(period), 0,
			       HRTIMER_MODE_REL_PINNED);
}

static void i915_pmu_timer_cancel(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;

	if (!is_sampling_event(event))
		return;

	local64_set(&hwc->period_left,
		    ktime_to_ns(hrtimer_get_remaining(&hwc->hrtimer)));
	hrtimer_cancel(&hwc->hrtimer);
}

static void i915_pmu_enable(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	unsigned long flags;

	spin_lock_irqsave(&i915->pmu.lock, flags);

	i915->pmu.enable |= BIT_ULL(event->attr.config);
	if (pmu_needs_timer(i915, true) && !i915->pmu.timer_enabled) {
		hrtimer_start_range_ns(&i915->pmu.timer,
				       ns_to_ktime(PERIOD), 0,
				       HRTIMER_MODE_REL_PINNED);
		i915->pmu.timer_enabled = true;
	}

	spin_unlock_irqrestore(&i915->pmu.lock, flags);

	i915_pmu_timer_start(event);
}

static void i915_pmu_disable(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	unsigned long flags;

	spin_lock_irqsave(&i915->pmu.lock, flags);
	i915->pmu.enable &= ~BIT_ULL(event->attr.config);
	i915->pmu.timer_enabled &= pmu_needs_timer(i915, true);
	spin_unlock_irqrestore(&i915->pmu.lock, flags);

	i915_pmu_timer_cancel(event);
}

static int i915_pmu_event_add(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	if (flags & PERF_EF_START)
		i915_pmu_enable(event);

	hwc->state = !(flags & PERF_EF_START);

	return 0;
}

static void i915_pmu_event_del(struct perf_event *event, int flags)
{
	i915_pmu_disable(event);
}

static void i915_pmu_event_start(struct perf_event *event, int flags)
{
	i915_pmu_enable(event);
}

static void i915_pmu_event_stop(struct perf_event *event, int flags)
{
	i915_pmu_disable(event);
}

static u64 read_energy_uJ(struct drm_i915_private *dev_priv)
{
	u64 power;

	GEM_BUG_ON(INTEL_GEN(dev_priv) < 6);

	intel_runtime_pm_get(dev_priv);

	rdmsrl(MSR_RAPL_POWER_UNIT, power);
	power = (power & 0x1f00) >> 8;
	power = 1000000 >> power; /* convert to uJ */
	power *= I915_READ_NOTRACE(MCH_SECP_NRG_STTS);

	intel_runtime_pm_put(dev_priv);

	return power;
}

static inline u64 calc_residency(struct drm_i915_private *dev_priv,
				 const i915_reg_t reg)
{
	u64 val, units = 128, div = 100000;

	GEM_BUG_ON(INTEL_GEN(dev_priv) < 6);

	intel_runtime_pm_get(dev_priv);
	if (IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv)) {
		div = dev_priv->czclk_freq;
		units = 1;
		if (I915_READ_NOTRACE(VLV_COUNTER_CONTROL) & VLV_COUNT_RANGE_HIGH)
			units <<= 8;
	} else if (IS_GEN9_LP(dev_priv)) {
		div = 1200;
		units = 1;
	}
	val = I915_READ_NOTRACE(reg);
	intel_runtime_pm_put(dev_priv);

	val *= units;
	return DIV_ROUND_UP_ULL(val, div);
}

static u64 count_interrupts(struct drm_i915_private *i915)
{
	/* open-coded kstat_irqs() */
	struct irq_desc *desc = irq_to_desc(i915->drm.pdev->irq);
	u64 sum = 0;
	int cpu;

	if (!desc || !desc->kstat_irqs)
		return 0;

	for_each_possible_cpu(cpu)
		sum += *per_cpu_ptr(desc->kstat_irqs, cpu);

	return sum;
}

static void i915_pmu_event_read(struct perf_event *event)
{
	struct drm_i915_private *i915 =
		container_of(event->pmu, typeof(*i915), pmu.base);
	u64 val = 0;

	if (event->attr.config < 32) {
		unsigned int user_engine = pmu_config_engine(event->attr.config);
		unsigned int sample = pmu_config_sampler(event->attr.config);

		if (WARN_ON_ONCE(user_engine >= ARRAY_SIZE(user_engine_map))) {
			/* Do nothing */
		} else {
			enum intel_engine_id id = user_engine_map[user_engine];
			val = i915->engine[id]->pmu_sample[sample];
		}
	} else switch (event->attr.config) {
	case I915_PMU_ACTUAL_FREQUENCY:
		val = i915->pmu.sample[__I915_SAMPLE_FREQ_ACT];
		break;
	case I915_PMU_REQUESTED_FREQUENCY:
		val = i915->pmu.sample[__I915_SAMPLE_FREQ_REQ];
		break;
	case I915_PMU_ENERGY:
		val = read_energy_uJ(i915);
		break;
	case I915_PMU_INTERRUPTS:
		val = count_interrupts(i915);
		break;

	case I915_PMU_RC6_RESIDENCY:
		if (!i915->gt.awake)
			return;

		val = calc_residency(i915, IS_VALLEYVIEW(i915) ? VLV_GT_RENDER_RC6 : GEN6_GT_GFX_RC6);
		break;

	case I915_PMU_RC6p_RESIDENCY:
		if (!i915->gt.awake)
			return;

		if (!IS_VALLEYVIEW(i915))
			val = calc_residency(i915, GEN6_GT_GFX_RC6p);
		break;

	case I915_PMU_RC6pp_RESIDENCY:
		if (!i915->gt.awake)
			return;

		if (!IS_VALLEYVIEW(i915))
			val = calc_residency(i915, GEN6_GT_GFX_RC6pp);
		break;
	}

	local64_set(&event->count, val);
}

static int i915_pmu_event_event_idx(struct perf_event *event)
{
	return 0;
}

static ssize_t i915_pmu_format_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
        struct dev_ext_attribute *eattr;

        eattr = container_of(attr, struct dev_ext_attribute, attr);
        return sprintf(buf, "%s\n", (char *) eattr->var);
}

#define I915_PMU_FORMAT_ATTR(_name, _config)           \
        (&((struct dev_ext_attribute[]) {               \
                { .attr = __ATTR(_name, S_IRUGO, i915_pmu_format_show, NULL), \
                  .var = (void *) _config, }            \
        })[0].attr.attr)

static struct attribute *i915_pmu_format_attrs[] = {
        I915_PMU_FORMAT_ATTR(i915_eventid, "config:0-42"),
        NULL,
};

static const struct attribute_group i915_pmu_format_attr_group = {
        .name = "format",
        .attrs = i915_pmu_format_attrs,
};

static ssize_t i915_pmu_event_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
        struct dev_ext_attribute *eattr;

        eattr = container_of(attr, struct dev_ext_attribute, attr);
        return sprintf(buf, "config=0x%lx\n", (unsigned long) eattr->var);
}

#define I915_PMU_EVENT_ATTR(_name, _config)            \
        (&((struct dev_ext_attribute[]) {               \
                { .attr = __ATTR(_name, S_IRUGO, i915_pmu_event_show, NULL), \
                  .var = (void *) _config, }            \
         })[0].attr.attr)

static struct attribute *i915_pmu_events_attrs[] = {
        I915_PMU_EVENT_ATTR(rcs-queued,	I915_PMU_COUNT_RCS_QUEUED),
        I915_PMU_EVENT_ATTR(rcs-busy,	I915_PMU_COUNT_RCS_BUSY),
        I915_PMU_EVENT_ATTR(rcs-wait,	I915_PMU_COUNT_RCS_WAIT),
        I915_PMU_EVENT_ATTR(rcs-sema,	I915_PMU_COUNT_RCS_SEMA),

        I915_PMU_EVENT_ATTR(bcs-queued,	I915_PMU_COUNT_BCS_QUEUED),
        I915_PMU_EVENT_ATTR(bcs-busy,	I915_PMU_COUNT_BCS_BUSY),
        I915_PMU_EVENT_ATTR(bcs-wait,	I915_PMU_COUNT_BCS_WAIT),
        I915_PMU_EVENT_ATTR(bcs-sema,	I915_PMU_COUNT_BCS_SEMA),

        I915_PMU_EVENT_ATTR(vcs-queued,	I915_PMU_COUNT_VCS_QUEUED),
        I915_PMU_EVENT_ATTR(vcs-busy,	I915_PMU_COUNT_VCS_BUSY),
        I915_PMU_EVENT_ATTR(vcs-wait,	I915_PMU_COUNT_VCS_WAIT),
        I915_PMU_EVENT_ATTR(vcs-sema,	I915_PMU_COUNT_VCS_SEMA),

        I915_PMU_EVENT_ATTR(vcs2-queued, I915_PMU_COUNT_VCS2_QUEUED),
        I915_PMU_EVENT_ATTR(vcs2-busy,	 I915_PMU_COUNT_VCS2_BUSY),
        I915_PMU_EVENT_ATTR(vcs2-wait,	 I915_PMU_COUNT_VCS2_WAIT),
        I915_PMU_EVENT_ATTR(vcs2-sema,	 I915_PMU_COUNT_VCS2_SEMA),

        I915_PMU_EVENT_ATTR(vecs-queued, I915_PMU_COUNT_VECS_QUEUED),
        I915_PMU_EVENT_ATTR(vecs-busy,	 I915_PMU_COUNT_VECS_BUSY),
        I915_PMU_EVENT_ATTR(vecs-wait,	 I915_PMU_COUNT_VECS_WAIT),
        I915_PMU_EVENT_ATTR(vecs-sema,	 I915_PMU_COUNT_VECS_SEMA),

        I915_PMU_EVENT_ATTR(actual-frequency,	 I915_PMU_ACTUAL_FREQUENCY),
        I915_PMU_EVENT_ATTR(requested-frequency, I915_PMU_REQUESTED_FREQUENCY),
        I915_PMU_EVENT_ATTR(energy,		 I915_PMU_ENERGY),
        I915_PMU_EVENT_ATTR(interrupts,		 I915_PMU_INTERRUPTS),
        I915_PMU_EVENT_ATTR(rc6-residency,	 I915_PMU_RC6_RESIDENCY),
        I915_PMU_EVENT_ATTR(rc6p-residency,	 I915_PMU_RC6p_RESIDENCY),
        I915_PMU_EVENT_ATTR(rc6pp-residency,	 I915_PMU_RC6pp_RESIDENCY),

        NULL,
};

static const struct attribute_group i915_pmu_events_attr_group = {
        .name = "events",
        .attrs = i915_pmu_events_attrs,
};

static const struct attribute_group *i915_pmu_attr_groups[] = {
        &i915_pmu_format_attr_group,
        &i915_pmu_events_attr_group,
        NULL
};

void i915_pmu_register(struct drm_i915_private *i915)
{
	if (INTEL_GEN(i915) <= 2)
		return;

	i915->pmu.base.attr_groups	= i915_pmu_attr_groups;
	i915->pmu.base.task_ctx_nr	= perf_sw_context;
	i915->pmu.base.event_init	= i915_pmu_event_init;
	i915->pmu.base.add		= i915_pmu_event_add;
	i915->pmu.base.del		= i915_pmu_event_del;
	i915->pmu.base.start		= i915_pmu_event_start;
	i915->pmu.base.stop		= i915_pmu_event_stop;
	i915->pmu.base.read		= i915_pmu_event_read;
	i915->pmu.base.event_idx	= i915_pmu_event_event_idx;

	spin_lock_init(&i915->pmu.lock);
	hrtimer_init(&i915->pmu.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	i915->pmu.timer.function = i915_sample;
	i915->pmu.enable = 0;

	if (perf_pmu_register(&i915->pmu.base, "i915", -1))
		i915->pmu.base.event_init = NULL;
}

void i915_pmu_unregister(struct drm_i915_private *i915)
{
	if (!i915->pmu.base.event_init)
		return;

	i915->pmu.enable = 0;

	perf_pmu_unregister(&i915->pmu.base);
	i915->pmu.base.event_init = NULL;

	hrtimer_cancel(&i915->pmu.timer);
}
