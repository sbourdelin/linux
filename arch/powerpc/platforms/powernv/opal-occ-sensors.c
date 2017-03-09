/*
 * Copyright IBM Corporation 2017
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "opal-occ-sensors: " fmt

#include <linux/of_platform.h>
#include <linux/miscdevice.h>
#include <linux/highmem.h>
#include <linux/slab.h>

#include <asm/opal.h>

enum sensor_structure_type {
	OCC_SENSOR_STRUCTURE_TYPE_FULL		= 0x01,
	OCC_SENSOR_STRUCTURE_TYPE_COUNTER	= 0x02,
};

enum occ_sensor_location {
	OCC_SENSOR_LOC_SYSTEM		= 0x0001,
	OCC_SENSOR_LOC_PROCESSOR	= 0x0002,
	OCC_SENSOR_LOC_PARTITION	= 0x0004,
	OCC_SENSOR_LOC_MEMORY		= 0x0008,
	OCC_SENSOR_LOC_VRM		= 0x0010,
	OCC_SENSOR_LOC_OCC		= 0x0020,
	OCC_SENSOR_LOC_CORE		= 0x0040,
	OCC_SENSOR_LOC_QUAD		= 0x0080,
	OCC_SENSOR_LOC_GPU		= 0x0100,
};

struct occ_sensor_name {
	char name[MAX_OCC_SENSOR_NAME_LEN];
	char units[MAX_OCC_SENSOR_UNITS_LEN];
	u16 gsid;
	u32 freq;
	u32 scale_factor;
	u16 type;
	u16 location;
	u8 structure_type;
	u32 reading_offset;
	u8 sensor_specific_info;
	u8 pad[];
} __packed;

struct occ_sensor_record {
	u16 gsid;
	u64 timestamp;
	u16 sample;
	u16 min;
	u16 max;
	u16 csm_min;
	u16 csm_max;
	u16 prof_min;
	u16 prof_max;
	u16 js_min;
	u16 js_max;
	u64 accumulator;
	u32 update_tag;
	u8 pad[];
} __packed;

struct occ_sensor_counter {
	u16 gsid;
	u64 timestamp;
	u64 accumulator;
	u8 sample;
	u8 pad[];
} __packed;

static struct occ_data {
	int id;
	int nr_sensors;
	u64 pbase;
	void *base;
	int names_offset;
	int ping_offset;
	int pong_offset;
} *occs;

static int nr_occs;
static int name_len;

static int opal_occ_sensor_get_count(enum occ_sensor_type type)
{
	struct occ_sensor_name *sensor;
	int count = 0;
	int i, j;

	for (i = 0; i < nr_occs; i++)
		for (j = 0; j < occs[i].nr_sensors; j++) {
			sensor = (struct occ_sensor_name *)(occs[i].base +
				 occs[i].names_offset + j * name_len);
			if (type == be16_to_cpu(sensor->type))
				count++;
		}

	return count;
}

struct occ_hwmon_sensor *opal_occ_sensor_get_hwmon_list(int *nr_sensors)
{
	struct occ_sensor_name *sensor;
	struct occ_hwmon_sensor *slist;
	int i, j, count = 0;

	*nr_sensors = opal_occ_sensor_get_count(OCC_SENSOR_TYPE_POWER);
	*nr_sensors += opal_occ_sensor_get_count(OCC_SENSOR_TYPE_TEMPERATURE);
	slist = kcalloc(*nr_sensors, sizeof(*slist), GFP_KERNEL);
	if (!slist)
		return NULL;

	for (i = 0; i < nr_occs; i++)
		for (j = 0; j < occs[i].nr_sensors; j++) {
			enum occ_sensor_type type;
			enum occ_sensor_location loc;

			sensor = (struct occ_sensor_name *)(occs[i].base +
				 occs[i].names_offset + j * name_len);
			type = be16_to_cpu(sensor->type);
			loc = be16_to_cpu(sensor->location);

			if (type != OCC_SENSOR_TYPE_POWER &&
			    type != OCC_SENSOR_TYPE_TEMPERATURE)
				continue;

			if (loc == OCC_SENSOR_LOC_SYSTEM)
				strncpy(slist[count].name, sensor->name,
					strlen(sensor->name));
			else
				snprintf(slist[count].name,
					 sizeof(slist[count].name), "P%d_%s",
					 occs[i].id, sensor->name);

			slist[count].type = type;
			slist[count].occ_id = occs[i].id;
			slist[count].offset =
					be32_to_cpu(sensor->reading_offset);
			count++;
		}

	return slist;
}
EXPORT_SYMBOL_GPL(opal_occ_sensor_get_hwmon_list);

static inline struct occ_data *get_occ(int occ_id)
{
	int i;

	for (i = 0; i < nr_occs; i++)
		if (occ_id == occs[i].id)
			return &occs[i];

	return NULL;
}

static struct occ_sensor_record *opal_occ_sensor_read_rec(int occ_id,
							  u64 offset)
{
	struct occ_sensor_record *sping, *spong;
	struct occ_data *occ;
	u8 *ping, *pong;

	occ = get_occ(occ_id);
	if (!occ)
		return NULL;

	ping = (u8 *)(occ->base + occ->ping_offset);
	pong = (u8 *)(occ->base + occ->pong_offset);
	sping = (struct occ_sensor_record *)((u64)ping + offset);
	spong = (struct occ_sensor_record *)((u64)pong + offset);

	if (*ping && *pong) {
		if (be64_to_cpu(sping->timestamp) >
		    be64_to_cpu(spong->timestamp))
			return sping;
		else
			return spong;
	} else if (*ping && !*pong) {
		return sping;
	} else if (*pong && !*pong) {
		return spong;
	} else if (!*ping && !*pong) {
		return NULL;
	}

	return NULL;
}

#define get(name)							\
int opal_occ_sensor_get_##name(int occ_id, u64 offset, u64 *val)	\
{									\
	struct occ_sensor_record *sensor;				\
	sensor = opal_occ_sensor_read_rec(occ_id, offset);		\
	if (!sensor)							\
		return -EIO;						\
	*val = be16_to_cpu(sensor->name);				\
	return 0;							\
}									\
EXPORT_SYMBOL_GPL(opal_occ_sensor_get_##name)

get(sample);
get(min);
get(max);
get(csm_min);
get(csm_max);
get(js_min);
get(js_max);
get(prof_min);
get(prof_max);

int __init opal_occ_sensors_init(void)
{
	struct platform_device *pdev;
	struct device_node *sensor, *node;
	int i, ret = -ENODEV;

	sensor = of_find_compatible_node(NULL, NULL,
					 "ibm,p9-occ-inband-sensor");
	if (!sensor) {
		pr_info("OCC inband sensors node not found\n");
		return ret;
	}

	for_each_child_of_node(sensor, node)
		if (strcmp(node->name, "occ") == 0)
			nr_occs++;

	if (of_property_read_u32(sensor, "sensor-names-size", &name_len)) {
		pr_info("Missing sensor-names-size DT property\n");
		return ret;
	}

	occs = kcalloc(nr_occs, sizeof(*occs), GFP_KERNEL);
	if (!occs)
		return -ENOMEM;

	i = 0;
	for_each_child_of_node(sensor, node) {
		const __be32 *reg;
		int reg_len = 0;

		if (strcmp(node->name, "occ") != 0)
			continue;

		if (of_property_read_u32(node, "ibm,occ-id", &occs[i].id)) {
			pr_info("Missing ibm,occ-id DT property\n");
			goto out;
		}

		if (of_property_read_u32(node, "nr-sensors",
					 &occs[i].nr_sensors)) {
			pr_info("Missing nr_sensors DT property\n");
			goto out;
		}

		if (of_property_read_u32(node, "ping-offset",
					 &occs[i].ping_offset)) {
			pr_info("Missing ping_offset DT property\n");
			goto out;
		}

		if (of_property_read_u32(node, "pong-offset",
					 &occs[i].pong_offset)) {
			pr_info("Missing pong_offset DT property\n");
			goto out;
		}

		if (of_property_read_u32(node, "names-offset",
					 &occs[i].names_offset)) {
			pr_info("Missing names_offset DT property\n");
			goto out;
		}

		reg = of_get_property(node, "reg", &reg_len);
		if (!reg_len) {
			pr_info("Missing reg DT property\n");
			goto out;
		}

		occs[i].pbase = be32_to_cpu(reg[0]);
		occs[i].pbase = be32_to_cpu(reg[1]) | (occs[i].pbase << 32);
		occs[i].base = phys_to_virt(occs[i].pbase);
		i++;
	}

	pdev = of_platform_device_create(sensor, "occ-inband-sensor", NULL);
	ret = PTR_ERR_OR_ZERO(pdev);
	if (!ret)
		return 0;
out:
	kfree(occs);
	return ret;
}
