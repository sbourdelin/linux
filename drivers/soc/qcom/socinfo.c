/*
 * Copyright (c) 2009-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/export.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sys_soc.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/random.h>
#include <linux/soc/qcom/smem.h>

#define PMIC_MODEL_UNKNOWN		0
#define HW_PLATFORM_QRD			11
#define PLATFORM_SUBTYPE_QRD_INVALID	6
#define PLATFORM_SUBTYPE_INVALID	4
/*
 * SOC version type with major number in the upper 16 bits and minor
 * number in the lower 16 bits.  For example:
 *   1.0 -> 0x00010000
 *   2.3 -> 0x00020003
 */
#define SOC_VERSION_MAJOR(ver) (((ver) & 0xffff0000) >> 16)
#define SOC_VERSION_MINOR(ver) ((ver) & 0x0000ffff)
#define SOCINFO_VERSION_MAJOR	SOC_VERSION_MAJOR
#define SOCINFO_VERSION_MINOR	SOC_VERSION_MINOR

#define SMEM_SOCINFO_BUILD_ID_LENGTH		32
#define SMEM_IMAGE_VERSION_BLOCKS_COUNT		32
#define SMEM_IMAGE_VERSION_SIZE			4096
#define SMEM_IMAGE_VERSION_NAME_SIZE		75
#define SMEM_IMAGE_VERSION_VARIANT_SIZE		20
#define SMEM_IMAGE_VERSION_OEM_SIZE		32
#define SMEM_IMAGE_VERSION_PARTITION_APPS	10

/*
 * SMEM item ids, used to acquire handles to respective
 * SMEM region.
 */
#define SMEM_IMAGE_VERSION_TABLE	469
#define SMEM_HW_SW_BUILD_ID		137

#define MAX_ATTR_COUNT	15

/*
 * SMEM Image table indices
 */
enum smem_image_table_index {
	SMEM_IMAGE_TABLE_BOOT_INDEX = 0,
	SMEM_IMAGE_TABLE_TZ_INDEX,
	SMEM_IMAGE_TABLE_RPM_INDEX = 3,
	SMEM_IMAGE_TABLE_APPS_INDEX = 10,
	SMEM_IMAGE_TABLE_MPSS_INDEX,
	SMEM_IMAGE_TABLE_ADSP_INDEX,
	SMEM_IMAGE_TABLE_CNSS_INDEX,
	SMEM_IMAGE_TABLE_VIDEO_INDEX
};

struct qcom_socinfo_attr {
	struct device_attribute attr;
	int min_version;
};

#define QCOM_SOCINFO_ATTR(_name, _show, _min_version) \
	{ __ATTR(_name, 0444, _show, NULL), _min_version }

#define SMEM_IMG_ATTR_ENTRY(_name, _mode, _show, _store, _index)	\
	struct dev_ext_attribute dev_attr_##_name =			\
	{ __ATTR(_name, _mode, _show, _store), (void *)_index }

#define QCOM_SMEM_IMG_ITEM(_name, _mode, _index)			\
	SMEM_IMG_ATTR_ENTRY(_name##_image_version, _mode,		\
		qcom_show_image_version, qcom_store_image_version,	\
		(unsigned long)_index);					\
	SMEM_IMG_ATTR_ENTRY(_name##_image_variant, _mode,		\
		qcom_show_image_variant, qcom_store_image_variant,	\
		(unsigned long)_index);					\
	SMEM_IMG_ATTR_ENTRY(_name##_image_crm, _mode,			\
		qcom_show_image_crm, qcom_store_image_crm,		\
		(unsigned long)_index);					\
static struct attribute *_name##_image_attrs[] = {			\
	&dev_attr_##_name##_image_version.attr.attr,			\
	&dev_attr_##_name##_image_variant.attr.attr,			\
	&dev_attr_##_name##_image_crm.attr.attr,			\
	NULL,								\
};									\
static struct attribute_group _name##_image_attr_group = {		\
	.attrs = _name##_image_attrs,					\
}

/* Hardware platform types */
static const char *const hw_platform[] = {
	[0] = "Unknown",
	[1] = "Surf",
	[2] = "FFA",
	[3] = "Fluid",
	[4] = "SVLTE_FFA",
	[5] = "SLVTE_SURF",
	[7] = "MDM_MTP_NO_DISPLAY",
	[8] = "MTP",
	[9] = "Liquid",
	[10] = "Dragon",
	[11] = "QRD",
	[13] = "HRD",
	[14] = "DTV",
	[21] = "RCM",
	[23] = "STP",
	[24] = "SBC",
};

static const char *const qrd_hw_platform_subtype[] = {
	[0] = "QRD",
	[1] = "SKUAA",
	[2] = "SKUF",
	[3] = "SKUAB",
	[5] = "SKUG",
	[6] = "INVALID",
};

static const char *const hw_platform_subtype[] = {
	"Unknown", "charm", "strange", "strange_2a", "Invalid",
};

static const char *const pmic_model[] = {
	[0]  = "Unknown PMIC model",
	[13] = "PMIC model: PM8058",
	[14] = "PMIC model: PM8028",
	[15] = "PMIC model: PM8901",
	[16] = "PMIC model: PM8027",
	[17] = "PMIC model: ISL9519",
	[18] = "PMIC model: PM8921",
	[19] = "PMIC model: PM8018",
	[20] = "PMIC model: PM8015",
	[21] = "PMIC model: PM8014",
	[22] = "PMIC model: PM8821",
	[23] = "PMIC model: PM8038",
	[24] = "PMIC model: PM8922",
	[25] = "PMIC model: PM8917",
};

struct smem_image_version {
	char name[SMEM_IMAGE_VERSION_NAME_SIZE];
	char variant[SMEM_IMAGE_VERSION_VARIANT_SIZE];
	char pad;
	char oem[SMEM_IMAGE_VERSION_OEM_SIZE];
};

/* Used to parse shared memory. Must match the modem. */
struct socinfo_v0_1 {
	u32 format;
	u32 id;
	u32 version;
	char build_id[SMEM_SOCINFO_BUILD_ID_LENGTH];
};

struct socinfo_v0_2 {
	struct socinfo_v0_1 v0_1;
	u32 raw_version;
};

struct socinfo_v0_3 {
	struct socinfo_v0_2 v0_2;
	u32 hw_platform;
};

struct socinfo_v0_4 {
	struct socinfo_v0_3 v0_3;
	u32 platform_version;
};

struct socinfo_v0_5 {
	struct socinfo_v0_4 v0_4;
	u32 accessory_chip;
};

struct socinfo_v0_6 {
	struct socinfo_v0_5 v0_5;
	u32 hw_platform_subtype;
};

struct socinfo_v0_7 {
	struct socinfo_v0_6 v0_6;
	u32 pmic_model;
	u32 pmic_die_revision;
};

struct socinfo_v0_8 {
	struct socinfo_v0_7 v0_7;
	u32 pmic_model_1;
	u32 pmic_die_revision_1;
	u32 pmic_model_2;
	u32 pmic_die_revision_2;
};

struct socinfo_v0_9 {
	struct socinfo_v0_8 v0_8;
	u32 foundry_id;
};

struct socinfo_v0_10 {
	struct socinfo_v0_9 v0_9;
	u32 serial_number;
};

struct socinfo_v0_11 {
	struct socinfo_v0_10 v0_10;
	u32 num_pmics;
	u32 pmic_array_offset;
};

struct socinfo_v0_12 {
	struct socinfo_v0_11 v0_11;
	u32 chip_family;
	u32 raw_device_family;
	u32 raw_device_number;
};

static union {
	struct socinfo_v0_1 v0_1;
	struct socinfo_v0_2 v0_2;
	struct socinfo_v0_3 v0_3;
	struct socinfo_v0_4 v0_4;
	struct socinfo_v0_5 v0_5;
	struct socinfo_v0_6 v0_6;
	struct socinfo_v0_7 v0_7;
	struct socinfo_v0_8 v0_8;
	struct socinfo_v0_9 v0_9;
	struct socinfo_v0_10 v0_10;
	struct socinfo_v0_11 v0_11;
	struct socinfo_v0_12 v0_12;
} *socinfo;

static struct smem_image_version *smem_image_version;
static u32 socinfo_format;

/* max socinfo format version supported */
#define MAX_SOCINFO_FORMAT 12

static const char *const cpu_of_id[] = {

	[0] = "Unknown CPU",

	/* 8x60 IDs */
	[87] = "MSM8960",

	/* 8x64 IDs */
	[109] = "APQ8064",
	[130] = "MPQ8064",

	/* 8x60A IDs */
	[122] = "MSM8660A",
	[123] = "MSM8260A",
	[124] = "APQ8060A",

	/* 8x74 IDs */
	[126] = "MSM8974",
	[184] = "APQ8074",
	[185] = "MSM8274",
	[186] = "MSM8674",

	/* 8x74AA IDs */
	[208] = "APQ8074-AA",
	[211] = "MSM8274-AA",
	[214] = "MSM8674-AA",
	[217] = "MSM8974-AA",

	/* 8x74AB IDs */
	[209] = "APQ8074-AB",
	[212] = "MSM8274-AB",
	[215] = "MSM8674-AB",
	[218] = "MSM8974-AB",

	/* 8x74AC IDs */
	[194] = "MSM8974PRO",
	[210] = "APQ8074PRO",
	[213] = "MSM8274PRO",
	[216] = "MSM8674PRO",

	/* 8x60AB IDs */
	[138] = "MSM8960AB",
	[139] = "APQ8060AB",
	[140] = "MSM8260AB",
	[141] = "MSM8660AB",

	/* 8x84 IDs */
	[178] = "APQ8084",

	/* 8x16 IDs */
	[206] = "MSM8916",
	[247] = "APQ8016",
	[248] = "MSM8216",
	[249] = "MSM8116",
	[250] = "MSM8616",

	/* 8x96 IDs */
	[246] = "MSM8996",
	[310] = "MSM8996AU",
	[311] = "APQ8096AU",
	[291] = "APQ8096",
	[305] = "MSM8996SG",
	[312] = "APQ8096SG",

	/*
	 * Uninitialized IDs are not known to run Linux.
	 * MSM_CPU_UNKNOWN is set to 0 to ensure these IDs are
	 * considered as unknown CPU.
	 */
};

/* socinfo: sysfs functions */

static ssize_t
qcom_show_vendor(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	return sprintf(buf, "%s", "Qualcomm\n");
}

static ssize_t
qcom_show_raw_version(struct device *dev,
		     struct device_attribute *attr,
		     char *buf)
{
	return sprintf(buf, "%u\n", socinfo->v0_2.raw_version);
}

static ssize_t
qcom_show_build_id(struct device *dev,
		   struct device_attribute *attr,
		   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", socinfo->v0_1.build_id);
}

static ssize_t
qcom_show_hw_platform(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%s\n", hw_platform[socinfo->v0_3.hw_platform]);
}

static ssize_t
qcom_show_platform_version(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", socinfo->v0_4.platform_version);
}

static ssize_t
qcom_show_accessory_chip(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", socinfo->v0_5.accessory_chip);
}

static ssize_t
qcom_show_platform_subtype(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	u32 hw_subtype = socinfo->v0_6.hw_platform_subtype;

	if (socinfo->v0_3.hw_platform == HW_PLATFORM_QRD)
		if (hw_subtype >= 0 &&
				hw_subtype < PLATFORM_SUBTYPE_QRD_INVALID)
			return sprintf(buf, "%s\n",
				qrd_hw_platform_subtype[hw_subtype]);
		else
			return -EINVAL;
	else
		if (hw_subtype >= 0 &&
				hw_subtype < PLATFORM_SUBTYPE_INVALID)
			return sprintf(buf, "%s\n",
				hw_platform_subtype[hw_subtype]);
		else
			return -EINVAL;
}

static ssize_t
qcom_show_platform_subtype_id(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%u\n", socinfo->v0_6.hw_platform_subtype);
}

static ssize_t
qcom_show_foundry_id(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%u\n", socinfo->v0_9.foundry_id);
}

static ssize_t
qcom_show_serial_number(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%u\n", socinfo->v0_10.serial_number);
}

static ssize_t
qcom_show_chip_family(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "0x%x\n", socinfo->v0_12.chip_family);
}

static ssize_t
qcom_show_raw_device_family(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "0x%x\n", socinfo->v0_12.raw_device_family);
}

static ssize_t
qcom_show_raw_device_number(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "0x%x\n", socinfo->v0_12.raw_device_number);
}

static ssize_t
qcom_show_pmic_model(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%s\n", pmic_model[socinfo->v0_7.pmic_model]);
}

static ssize_t
qcom_show_pmic_die_revision(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	return sprintf(buf, "%u\n", socinfo->v0_7.pmic_die_revision);
}

static ssize_t
qcom_show_image_version(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *dev_attr;
	int index;

	dev_attr = container_of(attr, struct dev_ext_attribute, attr);
	index = (unsigned long)(dev_attr->var);
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			smem_image_version[index].name);
}

static ssize_t
qcom_store_image_version(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct dev_ext_attribute *dev_attr;
	int index;

	dev_attr = container_of(attr, struct dev_ext_attribute, attr);
	index = (unsigned long)(dev_attr->var);
	return strlcpy(smem_image_version[index].name, buf,
			SMEM_IMAGE_VERSION_NAME_SIZE);
}

static ssize_t
qcom_show_image_variant(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *dev_attr;
	int index;

	dev_attr = container_of(attr, struct dev_ext_attribute, attr);
	index = (unsigned long)(dev_attr->var);
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			smem_image_version[index].variant);
}

static ssize_t
qcom_store_image_variant(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct dev_ext_attribute *dev_attr;
	int index;

	dev_attr = container_of(attr, struct dev_ext_attribute, attr);
	index = (unsigned long)(dev_attr->var);
	return strlcpy(smem_image_version[index].variant, buf,
			SMEM_IMAGE_VERSION_VARIANT_SIZE);
}

static ssize_t
qcom_show_image_crm(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dev_ext_attribute *dev_attr;
	int index;

	dev_attr = container_of(attr, struct dev_ext_attribute, attr);
	index = (unsigned long)(dev_attr->var);
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			smem_image_version[index].oem);
}

static ssize_t
qcom_store_image_crm(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct dev_ext_attribute *dev_attr;
	int index;

	dev_attr = container_of(attr, struct dev_ext_attribute, attr);
	index = (unsigned long)(dev_attr->var);
	return strlcpy(smem_image_version[index].oem, buf,
			SMEM_IMAGE_VERSION_OEM_SIZE);
}

static const struct qcom_socinfo_attr qcom_socinfo_attrs[] = {
	QCOM_SOCINFO_ATTR(chip_family, qcom_show_chip_family, 12),
	QCOM_SOCINFO_ATTR(raw_device_family, qcom_show_raw_device_family, 12),
	QCOM_SOCINFO_ATTR(raw_device_number, qcom_show_raw_device_number, 12),
	QCOM_SOCINFO_ATTR(serial_number, qcom_show_serial_number, 10),
	QCOM_SOCINFO_ATTR(foundry_id, qcom_show_foundry_id, 9),
	QCOM_SOCINFO_ATTR(pmic_model, qcom_show_pmic_model, 7),
	QCOM_SOCINFO_ATTR(pmic_die_revision, qcom_show_pmic_die_revision, 7),
	QCOM_SOCINFO_ATTR(platform_subtype, qcom_show_platform_subtype, 6),
	QCOM_SOCINFO_ATTR(platform_subtype_id,
				qcom_show_platform_subtype_id, 6),
	QCOM_SOCINFO_ATTR(accessory_chip, qcom_show_accessory_chip, 5),
	QCOM_SOCINFO_ATTR(platform_version, qcom_show_platform_version, 4),
	QCOM_SOCINFO_ATTR(hw_platform, qcom_show_hw_platform, 3),
	QCOM_SOCINFO_ATTR(raw_version, qcom_show_raw_version, 2),
	QCOM_SOCINFO_ATTR(build_id, qcom_show_build_id, 1),
	QCOM_SOCINFO_ATTR(vendor, qcom_show_vendor, 0),
};

QCOM_SMEM_IMG_ITEM(boot, 0444, SMEM_IMAGE_TABLE_BOOT_INDEX);
QCOM_SMEM_IMG_ITEM(tz, 0444, SMEM_IMAGE_TABLE_TZ_INDEX);
QCOM_SMEM_IMG_ITEM(rpm, 0444, SMEM_IMAGE_TABLE_RPM_INDEX);
QCOM_SMEM_IMG_ITEM(apps, 0644, SMEM_IMAGE_TABLE_APPS_INDEX);
QCOM_SMEM_IMG_ITEM(mpss, 0444, SMEM_IMAGE_TABLE_MPSS_INDEX);
QCOM_SMEM_IMG_ITEM(adsp, 0444, SMEM_IMAGE_TABLE_ADSP_INDEX);
QCOM_SMEM_IMG_ITEM(cnss, 0444, SMEM_IMAGE_TABLE_CNSS_INDEX);
QCOM_SMEM_IMG_ITEM(video, 0444, SMEM_IMAGE_TABLE_VIDEO_INDEX);

static struct attribute_group
	*smem_image_table[SMEM_IMAGE_VERSION_BLOCKS_COUNT] = {
		[SMEM_IMAGE_TABLE_BOOT_INDEX] = &boot_image_attr_group,
		[SMEM_IMAGE_TABLE_TZ_INDEX] = &tz_image_attr_group,
		[SMEM_IMAGE_TABLE_RPM_INDEX] = &rpm_image_attr_group,
		[SMEM_IMAGE_TABLE_APPS_INDEX] = &apps_image_attr_group,
		[SMEM_IMAGE_TABLE_MPSS_INDEX] = &mpss_image_attr_group,
		[SMEM_IMAGE_TABLE_ADSP_INDEX] = &adsp_image_attr_group,
		[SMEM_IMAGE_TABLE_CNSS_INDEX] = &cnss_image_attr_group,
		[SMEM_IMAGE_TABLE_VIDEO_INDEX] = &video_image_attr_group,
};

static void socinfo_populate_sysfs_files(struct device *dev)
{
	int idx;
	int err;

	/*
	 * Expose SMEM_IMAGE_TABLE to sysfs only when we have IMAGE_TABLE
	 * available in SMEM. As IMAGE_TABLE and SOCINFO are two separate
	 * items within SMEM, we expose the remaining soc information(i.e
	 * only the SOCINFO item available in SMEM) to sysfs even in the
	 * absence of an IMAGE_TABLE.
	 */
	if (smem_image_version)
		for (idx = 0; idx < SMEM_IMAGE_VERSION_BLOCKS_COUNT; idx++)
			if (smem_image_table[idx])
				err = sysfs_create_group(&dev->kobj,
					smem_image_table[idx]);

	for (idx = 0; idx < MAX_ATTR_COUNT; idx++)
		if (qcom_socinfo_attrs[idx].min_version <= socinfo_format)
			device_create_file(dev, &qcom_socinfo_attrs[idx].attr);
}

void qcom_socinfo_init(struct platform_device *pdev)
{
	struct soc_device_attribute *attr;
	struct device *qcom_soc_device;
	struct soc_device *soc_dev;
	size_t img_tbl_size;
	u32 soc_version;
	size_t size;

	socinfo = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_HW_SW_BUILD_ID,
			&size);
	if (IS_ERR(socinfo)) {
		dev_err(&pdev->dev, "Coudn't find socinfo.\n");
		return;
	}

	if ((SOCINFO_VERSION_MAJOR(socinfo->v0_1.format) != 0) ||
			(SOCINFO_VERSION_MINOR(socinfo->v0_1.format) < 0) ||
			(socinfo->v0_1.format > MAX_SOCINFO_FORMAT)) {
		dev_err(&pdev->dev, "Wrong socinfo format\n");
		return;
	}
	socinfo_format = socinfo->v0_1.format;

	if (!socinfo->v0_1.id)
		dev_err(&pdev->dev, "socinfo: Unknown SoC ID!\n");

	WARN(socinfo->v0_1.id >= ARRAY_SIZE(cpu_of_id),
		"New IDs added! ID => CPU mapping needs an update.\n");

	smem_image_version = qcom_smem_get(QCOM_SMEM_HOST_ANY,
				SMEM_IMAGE_VERSION_TABLE,
				&img_tbl_size);
	if (IS_ERR(smem_image_version) ||
		(img_tbl_size != SMEM_IMAGE_VERSION_SIZE)) {
		dev_warn(&pdev->dev, "Image version table absent.\n");
		smem_image_version = NULL;
	}

	attr = kzalloc(sizeof(*attr), GFP_KERNEL);
	if (!attr) {
		dev_err(&pdev->dev, "Unable to allocate attributes.\n");
		return;
	}
	soc_version = socinfo->v0_1.version;

	attr->soc_id = kasprintf(GFP_KERNEL, "%d", socinfo->v0_1.id);
	attr->family = "Snapdragon";
	attr->machine = cpu_of_id[socinfo->v0_1.id];
	attr->revision = kasprintf(GFP_KERNEL, "%u.%u",
				SOC_VERSION_MAJOR(soc_version),
				SOC_VERSION_MINOR(soc_version));

	soc_dev = soc_device_register(attr);
	if (IS_ERR(soc_dev)) {
		kfree(attr);
		return;
	}

	qcom_soc_device = soc_device_to_device(soc_dev);
	socinfo_populate_sysfs_files(qcom_soc_device);

	/* Feed the soc specific unique data into entropy pool */
	add_device_randomness(socinfo, size);
}
EXPORT_SYMBOL(qcom_socinfo_init);
