/*
 * Copyright(C) 2015 Linaro Limited. All rights reserved.
 * Author: Mathieu Poirier <mathieu.poirier@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/coresight.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "coresight-priv.h"

#define CORESIGHT_ETM_PMU_NAME  "cs_etm"

static struct pmu etm_pmu;
static bool etm_perf_up;

/**
 * struct etm_event_data - Coresight specifics associated to an event
 * @mask:		hold the CPU(s) this event was set for.
 * @source_config:	per CPU tracer configuration associated to a
 *			trace session.
 * @sink_config:	per CPU AUX configuration associated to a
 *			trace session.
 * @sink:		sink associated to a CPU.
 */
struct etm_event_data {
	cpumask_t mask;
	void **source_config;
	void **sink_config;
	void **sink;
};

static DEFINE_PER_CPU(struct perf_output_handle, ctx_handle);
static DEFINE_PER_CPU(struct coresight_device *, csdev_src);

/* ETMCR is 'config' */
PMU_FORMAT_ATTR(cycacc,		"config:12");
PMU_FORMAT_ATTR(timestamp,	"config:28");

static struct attribute *etm_config_formats_attr[] = {
	&format_attr_cycacc.attr,
	&format_attr_timestamp.attr,
	NULL,
};

static struct attribute_group etm_pmu_format_group = {
	.name   = "format",
	.attrs  = etm_config_formats_attr,
};

static const struct attribute_group *etm_pmu_attr_groups[] = {
	&etm_pmu_format_group,
	NULL,
};

static void etm_event_read(struct perf_event *event) {}

/**
 * etm_event_build_path() - setup a path between source and sink
 * @cpu:	The CPU the tracer is associated to.
 * @build:	Whether the path should be setup or thorned down.
 *
 * Return:	The _first_ sink buffer discovered during the walkthrough.
 */
static struct coresight_device *etm_event_build_path(int cpu, bool build)
{
	int ret = 0;
	LIST_HEAD(path);
	LIST_HEAD(sinks);
	struct coresight_device *csdev_source;
	struct coresight_device *csdev_sink = NULL;

	csdev_source = per_cpu(csdev_src, cpu);

	if (!csdev_source)
		return ERR_PTR(-EINVAL);

	if (csdev_source->type != CORESIGHT_DEV_TYPE_SOURCE)
		return ERR_PTR(-EINVAL);

	if (build) {
		ret = coresight_build_paths(csdev_source, &path, &sinks, build);
		if (ret) {
			dev_dbg(&csdev_source->dev,
				"creating path(s) failed\n");
			goto out;
		}

		/* Everything is good, record first enabled sink buffer */
		csdev_sink = list_first_entry(&sinks,
					      struct coresight_device, sinks);
	} else {
		ret = coresight_build_paths(csdev_source, &path, NULL, build);
		if (ret)
			dev_dbg(&csdev_source->dev,
				"releasing path(s) failed\n");
	}

out:
	return csdev_sink;
}

static int etm_event_pmu_start(struct perf_event *event)
{
	int cpu, ret;
	cpumask_t mask;
	struct coresight_device *csdev;

	cpumask_clear(&mask);
	if (event->cpu != -1)
		cpumask_set_cpu(event->cpu, &mask);
	else
		cpumask_copy(&mask, cpu_online_mask);

	for_each_cpu(cpu, &mask) {
		csdev = per_cpu(csdev_src, cpu);

		if (!source_ops(csdev)->perf_start)
			continue;

		ret = source_ops(csdev)->perf_start(csdev);
		if (ret)
			goto err;
	}

out:
	return ret;
err:
	for_each_cpu(cpu, &mask) {
		csdev = per_cpu(csdev_src, cpu);

		if (!source_ops(csdev)->perf_stop)
			continue;
		source_ops(csdev)->perf_stop(csdev);
	}

	goto out;
}

static void etm_event_destroy(struct perf_event *event)
{
	int cpu;
	cpumask_t mask;
	struct coresight_device *csdev;

	cpumask_clear(&mask);
	if (event->cpu != -1)
		cpumask_set_cpu(event->cpu, &mask);
	else
		cpumask_copy(&mask, cpu_online_mask);

	for_each_cpu(cpu, &mask) {
		csdev = per_cpu(csdev_src, cpu);
		etm_event_build_path(cpu, false);

		if (!source_ops(csdev)->perf_stop)
			continue;
		source_ops(csdev)->perf_stop(csdev);
	}
}

static int etm_event_init(struct perf_event *event)
{
	int ret;

	if (event->attr.type != etm_pmu.type)
		return -ENOENT;

	if (event->cpu >= nr_cpu_ids)
		return -EINVAL;

	ret = etm_event_pmu_start(event);
	if (ret)
		return ret;

	event->destroy = etm_event_destroy;

	return 0;
}

static void *alloc_event_data(int cpu)
{
	int size;
	struct etm_event_data *event_data;
	void *source_config, *sink_config, *sink;

	event_data = kzalloc(sizeof(struct etm_event_data), GFP_KERNEL);
	 if (!event_data)
		return NULL;

	if (cpu != -1)
		size = 1;
	else
		size = num_online_cpus();

	source_config = kcalloc(size, sizeof(void *), GFP_KERNEL);
	if (!source_config)
		goto source_config_err;

	sink_config = kcalloc(size, sizeof(void *), GFP_KERNEL);
	if (!sink_config)
		goto sink_config_err;

	sink = kcalloc(size, sizeof(void *), GFP_KERNEL);
	if (!sink)
		goto sink_err;

	cpumask_clear(&event_data->mask);
	event_data->source_config = source_config;
	event_data->sink_config = sink_config;
	event_data->sink = sink;

out:
	return event_data;

sink_err:
	kfree(sink_config);
sink_config_err:
	kfree(source_config);
source_config_err:
	kfree(event_data);
	event_data = NULL;
	goto out;
}

static void free_event_data(struct etm_event_data *event_data)
{
	int cpu;
	cpumask_t *mask = &event_data->mask;

	for_each_cpu(cpu, mask) {
		kfree(event_data->source_config[cpu]);
		kfree(event_data->sink_config[cpu]);
		kfree(event_data->sink[cpu]);
	}

	kfree(event_data->source_config);
	kfree(event_data->sink_config);
	kfree(event_data->sink);
	kfree(event_data);
}

static void *etm_setup_aux(struct perf_event *event, void **pages,
			   int nr_pages, bool overwrite)
{
	int cpu;
	cpumask_t *mask;
	struct etm_event_data *event_data = NULL;
	struct coresight_device *csdev;

	event_data = alloc_event_data(event->cpu);
	if (!event_data)
		return NULL;

	mask = &event_data->mask;

	if (event->cpu != -1)
		cpumask_set_cpu(event->cpu, mask);
	else
		cpumask_copy(mask, cpu_online_mask);

	for_each_cpu(cpu, mask) {
		struct coresight_device *sink;

		csdev = per_cpu(csdev_src, cpu);
		if (!csdev)
			goto err;

		/* Get the tracer's config from perf */
		if (!source_ops(csdev)->perf_get_config)
			goto err;

		event_data->source_config[cpu] =
			source_ops(csdev)->perf_get_config(csdev, event);

		if (!event_data->source_config[cpu])
			goto err;

		/*
		 * Get a handle on the sink buffer associated
		 * with this tracer.
		 */
		event_data->sink[cpu] = (void *)etm_event_build_path(cpu, true);

		if (!event_data->sink[cpu])
			goto err;

		sink = event_data->sink[cpu];

		if (!sink_ops(sink)->setup_aux)
			goto err;

		/* Finally get the AUX specific data from the sink buffer */
		event_data->sink_config[cpu] =
				sink_ops(sink)->setup_aux(sink, cpu, pages,
							  nr_pages, overwrite);
		if (!event_data->sink_config[cpu])
			goto err;
	}

out:
	return event_data;

err:
	for_each_cpu(cpu, mask) {
		etm_event_build_path(cpu, false);
	}

	free_event_data(event_data);
	event_data = NULL;
	goto out;
}

static void etm_free_aux(void *data)
{
	free_event_data(data);
}

static void etm_event_stop(struct perf_event *event, int mode)
{
	int cpu = smp_processor_id();
	struct coresight_device *csdev = per_cpu(csdev_src, cpu);

	if (event->hw.state == PERF_HES_STOPPED)
		return;

	if (!csdev)
		return;

	/* stop tracer */
	if (!source_ops(csdev)->perf_disable)
		return;

	if (source_ops(csdev)->perf_disable(csdev))
		return;

	/* tell the core */
	event->hw.state = PERF_HES_STOPPED;


	if (mode & PERF_EF_UPDATE) {
		struct coresight_device *sink;
		struct perf_output_handle *handle = this_cpu_ptr(&ctx_handle);
		struct etm_event_data *event_data = perf_get_aux(handle);

		if (WARN_ON_ONCE(handle->event != event))
			return;

		if (WARN_ON_ONCE(!event_data))
			return;

		sink = event_data->sink[cpu];
		if (WARN_ON_ONCE(!sink))
			return;

		/* update trace information */
		if (!sink_ops(sink)->update_buffer)
			return;

		sink_ops(sink)->update_buffer(sink, handle,
					      event_data->sink_config[cpu]);
	}
}

static void etm_event_start(struct perf_event *event, int flags)
{
	int cpu = smp_processor_id();
	struct coresight_device *csdev = per_cpu(csdev_src, cpu);

	if (!csdev)
		goto fail;

	/* tell the perf core the event is alive */
	event->hw.state = 0;

	if (!source_ops(csdev)->perf_enable)
		goto fail;

	if (source_ops(csdev)->perf_enable(csdev))
		goto fail;

	return;

fail:
	event->hw.state = PERF_HES_STOPPED;
}

static void etm_event_del(struct perf_event *event, int mode)
{
	int cpu = smp_processor_id();
	struct coresight_device *sink;
	struct perf_output_handle *handle = this_cpu_ptr(&ctx_handle);
	struct etm_event_data *event_data = perf_get_aux(handle);

	if (WARN_ON_ONCE(!event_data))
		return;

	sink = event_data->sink[cpu];
	if (!sink)
		return;

	etm_event_stop(event, PERF_EF_UPDATE);

	if (!sink_ops(sink)->reset_buffer)
		return;

	sink_ops(sink)->reset_buffer(sink, handle,
				     event_data->sink_config[cpu]);
}

static int etm_event_add(struct perf_event *event, int mode)
{

	int ret = -EBUSY, cpu = smp_processor_id();
	struct etm_event_data *event_data;
	struct perf_output_handle *handle = this_cpu_ptr(&ctx_handle);
	struct hw_perf_event *hwc = &event->hw;
	struct coresight_device *csdev = per_cpu(csdev_src, cpu);
	struct coresight_device *sink;

	if (handle->event)
		goto out;

	event_data = perf_aux_output_begin(handle, event);
	ret = -EINVAL;
	if (WARN_ON_ONCE(!event_data))
		goto fail_stop;

	sink = event_data->sink[cpu];
	if (!sink)
		goto fail_end_stop;

	if (!sink_ops(sink)->set_buffer)
		goto fail_end_stop;

	ret = sink_ops(sink)->set_buffer(sink, handle,
					 event_data->sink_config[cpu]);
	if (ret)
		goto fail_end_stop;

	if (!source_ops(csdev)->perf_set_config) {
		ret = -EINVAL;
		goto fail_end_stop;
	}

	source_ops(csdev)->perf_set_config(csdev,
					   event_data->source_config[cpu]);

	if (mode & PERF_EF_START) {
		etm_event_start(event, 0);
		if (hwc->state & PERF_HES_STOPPED) {
			etm_event_del(event, 0);
			return -EBUSY;
		}
	}

out:
	return ret;

fail_end_stop:
	perf_aux_output_end(handle, 0, true);
fail_stop:
	hwc->state = PERF_HES_STOPPED;
	goto out;
}

int etm_perf_symlink(struct coresight_device *csdev, bool link)
{
	char entry[sizeof("cpu9999999")];
	int ret = 0, cpu = source_ops(csdev)->cpu_id(csdev);
	struct device *pmu_dev = etm_pmu.dev;
	struct device *cs_dev = &csdev->dev;

	sprintf(entry, "cpu%d", cpu);

	if (!etm_perf_up)
		return -EPROBE_DEFER;

	if (link) {
		ret = sysfs_create_link(&pmu_dev->kobj, &cs_dev->kobj, entry);
		if (ret)
			return ret;
		per_cpu(csdev_src, cpu) = csdev;
	} else {
		sysfs_remove_link(&pmu_dev->kobj, entry);
		per_cpu(csdev_src, cpu) = NULL;
	}

	return 0;
}

static int __init etm_perf_init(void)
{
	int ret;

	etm_pmu.capabilities	= PERF_PMU_CAP_EXCLUSIVE;

	etm_pmu.attr_groups	= etm_pmu_attr_groups;
	etm_pmu.task_ctx_nr	= perf_sw_context;
	etm_pmu.read		= etm_event_read;
	etm_pmu.event_init	= etm_event_init;
	etm_pmu.setup_aux	= etm_setup_aux;
	etm_pmu.free_aux	= etm_free_aux;
	etm_pmu.stop		= etm_event_stop;
	etm_pmu.start		= etm_event_start;
	etm_pmu.del		= etm_event_del;
	etm_pmu.add		= etm_event_add;

	ret = perf_pmu_register(&etm_pmu, CORESIGHT_ETM_PMU_NAME, -1);
	if (ret == 0)
		etm_perf_up = true;

	return ret;
}
module_init(etm_perf_init);
