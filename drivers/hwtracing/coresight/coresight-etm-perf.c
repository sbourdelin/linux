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
#include <linux/coresight-pmu.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "coresight-priv.h"

static struct pmu etm_pmu;
static bool etm_perf_up;

/**
 * struct etm_cpu_data - Coresight specifics accociated to a single CPU
 * @src_config:		The tracer configuration.
 * @snk_config:		The sink configuration.
 * @patch:		The path from source to sink.
 */
struct etm_cpu_data {
	void *src_config;
	void *snk_config;
	struct list_head *path;
};

/**
 * struct etm_event_data - Coresight specifics associated to an event
 * @mask:		Hold the CPU(s) this event was set for.
 * @cpu_data:		An array of cpu data, each slot for one CPU.
 */
struct etm_event_data {
	cpumask_t mask;
	struct etm_cpu_data **cpu_data;
};

static DEFINE_PER_CPU(struct perf_output_handle, ctx_handle);
static DEFINE_PER_CPU(struct coresight_device *, csdev_src);

/* ETMv3.5/PTM's ETMCR is 'config' */
PMU_FORMAT_ATTR(cycacc,		"config:" __stringify(ETM_OPT_CYCACC));
PMU_FORMAT_ATTR(timestamp,	"config:" __stringify(ETM_OPT_TS));

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

static int etm_event_init(struct perf_event *event)
{
	if (event->attr.type != etm_pmu.type)
		return -ENOENT;

	if (event->cpu >= nr_cpu_ids)
		return -EINVAL;

	return 0;
}

static void free_cpu_data(int cpu, struct etm_cpu_data *cpu_data)
{
	struct coresight_device *sink, *csdev;

	csdev = per_cpu(csdev_src, cpu);
	if (!csdev)
		return;

	if (source_ops(csdev)->put_config)
		source_ops(csdev)->put_config(cpu_data->src_config);

	/* No need to continue if there isn't a path to work with */
	if (!cpu_data->path)
		return;

	sink = coresight_get_sink(cpu_data->path);
	if (sink_ops(sink)->put_config)
		sink_ops(sink)->put_config(cpu_data->snk_config);

	coresight_release_path(cpu_data->path);
}

static void free_event_data(struct etm_event_data *event_data)
{
	int cpu;
	cpumask_t *mask = &event_data->mask;

	for_each_cpu(cpu, mask) {
		if (event_data->cpu_data[cpu])
			free_cpu_data(cpu, event_data->cpu_data[cpu]);
		kfree(event_data->cpu_data[cpu]);
	}

	kfree(event_data->cpu_data);
	kfree(event_data);
}

static void *alloc_event_data(int cpu)
{
	int lcpu, size;
	cpumask_t *mask;
	struct etm_cpu_data *cpu_data;
	struct etm_event_data *event_data;

	/* First get memory for the session's data */
	event_data = kzalloc(sizeof(struct etm_event_data), GFP_KERNEL);
	if (!event_data)
		return NULL;

	/* Make sure nothing disappears under us */
	get_online_cpus();
	size = num_online_cpus();

	mask = &event_data->mask;
	if (cpu != -1)
		cpumask_set_cpu(cpu, mask);
	else
		cpumask_copy(mask, cpu_online_mask);
	put_online_cpus();

	/*
	 * Allocate an array of cpu_data to work with.  That array is mainly
	 * serving as a quick way of retrieving trace configuration data
	 * associated to each CPU by using the number of those CPUs as
	 * indexes.  The expense of unused memory when dealing with single
	 * CPU trace scenarios is small compared to the search cost of
	 * using an optimized array size.
	 */
	event_data->cpu_data = kcalloc(size,
				       sizeof(struct etm_cpu_data *),
				       GFP_KERNEL);
	if (!event_data->cpu_data)
		goto free_event_data;

	/* Allocate a cpu_data for each CPU this event is dealing with */
	for_each_cpu(lcpu, mask) {
		cpu_data = kzalloc(sizeof(struct etm_cpu_data), GFP_KERNEL);
		if (!cpu_data)
			goto free_event_data;

		event_data->cpu_data[lcpu] = cpu_data;
	}

out:
	return event_data;

free_event_data:
	free_event_data(event_data);
	event_data = NULL;

	goto out;
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

	for_each_cpu(cpu, mask) {
		struct etm_cpu_data *cpu_data;
		struct coresight_device *sink;

		csdev = per_cpu(csdev_src, cpu);
		if (!csdev)
			goto err;

		cpu_data = event_data->cpu_data[cpu];

		/* Get the tracer's config from perf */
		if (!source_ops(csdev)->get_config)
			goto err;

		/*
		 * Since CPUs can be associated with different tracers on the
		 * same SoC and that tracers have different ways of
		 * configuring trace options, parse and collect each CPU's
		 * configuration before the trace run starts.  That way the
		 * parsing/processing of options happens only once and not
		 * on the fast path.
		 */
		cpu_data->src_config =
				source_ops(csdev)->get_config(csdev, event);
		if (!cpu_data->src_config)
			goto err;

		/*
		 * Building a path doesn't enable it, it simply builds a
		 * list of devices from source to sink that can be
		 * referenced later when the path is actually needed.
		 */
		cpu_data->path = coresight_build_path(csdev);
		if (!cpu_data->path)
			goto err;

		/* Grab the sink at the end of the path */
		sink = coresight_get_sink(cpu_data->path);
		if (!sink)
			goto err;

		if (!sink_ops(sink)->get_config)
			goto err;

		/* Finally get the AUX specific data from the sink buffer */
		cpu_data->snk_config =
				sink_ops(sink)->get_config(sink, cpu, pages,
							   nr_pages, overwrite);
		if (!cpu_data->snk_config)
			goto err;

	}

out:
	return event_data;

err:
	free_event_data(event_data);
	event_data = NULL;
	goto out;

	return NULL;
}

static void etm_free_aux(void *data)
{
	free_event_data(data);
}

static void etm_event_stop(struct perf_event *event, int mode)
{
	bool lost;
	int cpu = smp_processor_id();
	unsigned long size;
	struct coresight_device *sink, *csdev = per_cpu(csdev_src, cpu);
	struct perf_output_handle *handle = this_cpu_ptr(&ctx_handle);
	struct etm_event_data *event_data = perf_get_aux(handle);
	struct etm_cpu_data *cpu_data = event_data->cpu_data[cpu];

	if (event->hw.state == PERF_HES_STOPPED)
		return;

	if (!csdev || !cpu_data)
		return;

	sink = coresight_get_sink(cpu_data->path);
	if (!sink)
		return;

	/* stop tracer */
	source_ops(csdev)->disable(csdev);

	/* tell the core */
	event->hw.state = PERF_HES_STOPPED;

	if (mode & PERF_EF_UPDATE) {
		if (WARN_ON_ONCE(handle->event != event))
			return;

		/* update trace information */
		if (!sink_ops(sink)->update_buffer)
			return;

		sink_ops(sink)->update_buffer(sink, handle,
					      cpu_data->snk_config);

		if (!sink_ops(sink)->reset_buffer)
			return;

		size = sink_ops(sink)->reset_buffer(sink, handle,
						    cpu_data->snk_config,
						    &lost);

		perf_aux_output_end(handle, size, lost);
	}

	/* Disabling the path make its elements available to other sessions */
	coresight_disable_path(cpu_data->path);
}

static void etm_event_start(struct perf_event *event, int flags)
{
	int cpu = smp_processor_id();
	struct etm_cpu_data *cpu_data;
	struct etm_event_data *event_data;
	struct perf_output_handle *handle = this_cpu_ptr(&ctx_handle);
	struct coresight_device *sink, *csdev = per_cpu(csdev_src, cpu);

	if (!csdev)
		goto fail;

	/*
	 * Deal with the ring buffer API and get a handle on the
	 * session's information.
	 */
	event_data = perf_aux_output_begin(handle, event);
	if (WARN_ON_ONCE(!event_data))
		goto fail;

	/* Get the session information for this CPU */
	cpu_data = event_data->cpu_data[cpu];

	/* We need a sink, no need to continue without one */
	sink = coresight_get_sink(cpu_data->path);
	if (!sink || !sink_ops(sink)->set_buffer)
		goto fail_end_stop;

	/* Configure the sink */
	if (sink_ops(sink)->set_buffer(sink, handle,
				       cpu_data->snk_config))
		goto fail_end_stop;

	if (!source_ops(csdev)->set_config)
		goto fail_end_stop;

	/* Configure the tracer */
	source_ops(csdev)->set_config(csdev, cpu_data->src_config);

	/* Nothing will happen without a path */
	if (coresight_enable_path(cpu_data->path, CS_MODE_PERF))
		goto fail_end_stop;

	/* Tell the perf core the event is alive */
	event->hw.state = 0;

	/* Finally enable the tracer */
	if (source_ops(csdev)->enable(csdev, CS_MODE_PERF))
		goto fail_end_stop;

out:
	return;

fail_end_stop:
	perf_aux_output_end(handle, 0, true);
fail:
	event->hw.state = PERF_HES_STOPPED;
	goto out;
}

static void etm_event_del(struct perf_event *event, int mode)
{
	etm_event_stop(event, PERF_EF_UPDATE);
}

static int etm_event_add(struct perf_event *event, int mode)
{
	int ret = 0;
	struct hw_perf_event *hwc = &event->hw;

	if (mode & PERF_EF_START) {
		etm_event_start(event, 0);
		if (hwc->state & PERF_HES_STOPPED)
			ret = -EINVAL;
	} else {
		hwc->state = PERF_HES_STOPPED;
	}

	return ret;
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
