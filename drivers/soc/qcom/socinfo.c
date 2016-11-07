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

#define PMIC_MODEL_UNKNOWN 0
#define HW_PLATFORM_QRD 11
#define PLATFORM_SUBTYPE_QRD_INVALID 6
#define PLATFORM_SUBTYPE_INVALID 4
/*
 * SOC version type with major number in the upper 16 bits and minor
 * number in the lower 16 bits.  For example:
 *   1.0 -> 0x00010000
 *   2.3 -> 0x00020003
 */
#define SOC_VERSION_MAJOR(ver) (((ver) & 0xffff0000) >> 16)
#define SOC_VERSION_MINOR(ver) ((ver) & 0x0000ffff)
#define SOCINFO_FORMAT(x)  (x)
#define SOCINFO_VERSION_MAJOR	SOC_VERSION_MAJOR
#define SOCINFO_VERSION_MINOR	SOC_VERSION_MINOR

#define SMEM_SOCINFO_BUILD_ID_LENGTH 32
#define SMEM_IMAGE_VERSION_BLOCKS_COUNT 32
#define SMEM_IMAGE_VERSION_SIZE 4096
#define SMEM_IMAGE_VERSION_NAME_SIZE 75
#define SMEM_IMAGE_VERSION_VARIANT_SIZE 20
#define SMEM_IMAGE_VERSION_OEM_SIZE 32
#define SMEM_IMAGE_VERSION_PARTITION_APPS 10

/*
 * Shared memory identifiers, used to acquire handles to respective memory
 * region.
 */
#define SMEM_IMAGE_VERSION_TABLE	469

/*
 * Shared memory identifiers, used to acquire handles to respective memory
 * region.
 */
#define SMEM_HW_SW_BUILD_ID		137

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

#define SMEM_IMAGE_TABLE_ATTR(type, index)				\
static ssize_t								\
qcom_get_##type##_image_version(struct device *dev,			\
			struct device_attribute *attr,			\
			char *buf)					\
{									\
	return	qcom_get_image_version(index, buf);			\
}									\
static ssize_t								\
qcom_set_##type##_image_version(struct device *dev,			\
			struct device_attribute *attr,			\
			const char *buf,				\
			size_t count)					\
{									\
	return	qcom_set_image_version(index, buf);			\
}									\
static ssize_t								\
qcom_get_##type##_image_variant(struct device *dev,			\
			struct device_attribute *attr,			\
			char *buf)					\
{									\
	return	qcom_get_image_variant(index, buf);			\
}									\
static ssize_t								\
qcom_set_##type##_image_variant(struct device *dev,			\
			struct device_attribute *attr,			\
			const char *buf,				\
			size_t count)					\
{									\
	return	qcom_set_image_variant(index, buf);			\
}									\
static ssize_t								\
qcom_get_##type##_image_crm_version(struct device *dev,			\
			struct device_attribute *attr,			\
			char *buf)					\
{									\
	return	qcom_get_image_crm_version(index, buf);			\
}									\
static ssize_t								\
qcom_set_##type##_image_crm_version(struct device *dev,			\
			struct device_attribute *attr,			\
			const char *buf,				\
			size_t count)					\
{									\
	return	qcom_set_image_crm_version(index, buf);			\
}									\
DEVICE_ATTR(type##_image_version, 0644, qcom_get_##type##_image_version,\
				qcom_set_##type##_image_version);	\
DEVICE_ATTR(type##_image_variant, 0644, qcom_get_##type##_image_variant,\
				qcom_set_##type##_image_variant);	\
DEVICE_ATTR(type##_image_crm_version, 0644,				\
				qcom_get_##type##_image_crm_version,	\
				qcom_set_##type##_image_crm_version);	\
static struct attribute *type##_image_attrs[] = {			\
	&dev_attr_##type##_image_version.attr,				\
	&dev_attr_##type##_image_variant.attr,				\
	&dev_attr_##type##_image_crm_version.attr,			\
	NULL,								\
};									\
static struct attribute_group type##_image_attr_group = {		\
	.attrs = type##_image_attrs,					\
}

#define SMEM_IMAGE_TABLE_RO_ATTR(type, index)				\
static ssize_t								\
qcom_get_##type##_image_version(struct device *dev,			\
			struct device_attribute *attr,			\
			char *buf)					\
{									\
	return	qcom_get_image_version(index, buf);			\
}									\
static ssize_t								\
qcom_get_##type##_image_variant(struct device *dev,			\
			struct device_attribute *attr,			\
			char *buf)					\
{									\
	return	qcom_get_image_variant(index, buf);			\
}									\
static ssize_t								\
qcom_get_##type##_image_crm_version(struct device *dev,			\
			struct device_attribute *attr,			\
			char *buf)					\
{									\
	return	qcom_get_image_crm_version(index, buf);			\
}									\
DEVICE_ATTR(type##_image_version, 0444, qcom_get_##type##_image_version,\
					NULL);				\
DEVICE_ATTR(type##_image_variant, 0444, qcom_get_##type##_image_variant,\
					NULL);				\
DEVICE_ATTR(type##_image_crm_version, 0444,				\
				qcom_get_##type##_image_crm_version,	\
					NULL);				\
static struct attribute *type##_image_attrs[] = {			\
	&dev_attr_##type##_image_version.attr,				\
	&dev_attr_##type##_image_variant.attr,				\
	&dev_attr_##type##_image_crm_version.attr,			\
	NULL,								\
	};								\
static struct attribute_group type##_image_attr_group = {		\
	.attrs = type##_image_attrs,					\
}

/*
 * Qcom SoC types
 */
enum qcom_cpu {
	MSM_CPU_UNKNOWN = 0,
	MSM_CPU_8960,
	MSM_CPU_8960AB,
	MSM_CPU_8064,
	MSM_CPU_8974,
	MSM_CPU_8974PRO_AA,
	MSM_CPU_8974PRO_AB,
	MSM_CPU_8974PRO_AC,
	MSM_CPU_8916,
	MSM_CPU_8084,
	MSM_CPU_8996
};

struct qcom_soc_info {
	enum qcom_cpu generic_soc_type;
	char *soc_id_string;
};

/* Hardware platform types */
static const char *hw_platform[] = {
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

static const char *qrd_hw_platform_subtype[] = {
	[0] = "QRD",
	[1] = "SKUAA",
	[2] = "SKUF",
	[3] = "SKUAB",
	[5] = "SKUG",
	[6] = "INVALID",
};

static const char *hw_platform_subtype[] = {
	"Unknown", "charm", "strange", "strange_2a", "Invalid",
};

static const char *pmic_model[] = {
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
static bool smem_img_tbl_avlbl = true;
static bool hw_subtype_valid = true;
static u32 hw_subtype;


/* max socinfo format version supported */
#define MAX_SOCINFO_FORMAT SOCINFO_FORMAT(12)

static struct qcom_soc_info cpu_of_id[] = {

	[0] = {MSM_CPU_UNKNOWN, "Unknown CPU"},

	/* 8x60 IDs */
	[87] = {MSM_CPU_8960, "MSM8960"},

	/* 8x64 IDs */
	[109] = {MSM_CPU_8064, "APQ8064"},
	[130] = {MSM_CPU_8064, "MPQ8064"},

	/* 8x60A IDs */
	[122] = {MSM_CPU_8960, "MSM8660A"},
	[123] = {MSM_CPU_8960, "MSM8260A"},
	[124] = {MSM_CPU_8960, "APQ8060A"},

	/* 8x74 IDs */
	[126] = {MSM_CPU_8974, "MSM8974"},
	[184] = {MSM_CPU_8974, "APQ8074"},
	[185] = {MSM_CPU_8974, "MSM8274"},
	[186] = {MSM_CPU_8974, "MSM8674"},

	/* 8x74AA IDs */
	[208] = {MSM_CPU_8974PRO_AA, "APQ8074-AA"},
	[211] = {MSM_CPU_8974PRO_AA, "MSM8274-AA"},
	[214] = {MSM_CPU_8974PRO_AA, "MSM8674-AA"},
	[217] = {MSM_CPU_8974PRO_AA, "MSM8974-AA"},

	/* 8x74AB IDs */
	[209] = {MSM_CPU_8974PRO_AB, "APQ8074-AB"},
	[212] = {MSM_CPU_8974PRO_AB, "MSM8274-AB"},
	[215] = {MSM_CPU_8974PRO_AB, "MSM8674-AB"},
	[218] = {MSM_CPU_8974PRO_AB, "MSM8974-AB"},

	/* 8x74AC IDs */
	[194] = {MSM_CPU_8974PRO_AC, "MSM8974PRO"},
	[210] = {MSM_CPU_8974PRO_AC, "APQ8074PRO"},
	[213] = {MSM_CPU_8974PRO_AC, "MSM8274PRO"},
	[216] = {MSM_CPU_8974PRO_AC, "MSM8674PRO"},

	/* 8x60AB IDs */
	[138] = {MSM_CPU_8960AB, "MSM8960AB"},
	[139] = {MSM_CPU_8960AB, "APQ8060AB"},
	[140] = {MSM_CPU_8960AB, "MSM8260AB"},
	[141] = {MSM_CPU_8960AB, "MSM8660AB"},

	/* 8x84 IDs */
	[178] = {MSM_CPU_8084, "APQ8084"},

	/* 8x16 IDs */
	[206] = {MSM_CPU_8916, "MSM8916"},
	[247] = {MSM_CPU_8916, "APQ8016"},
	[248] = {MSM_CPU_8916, "MSM8216"},
	[249] = {MSM_CPU_8916, "MSM8116"},
	[250] = {MSM_CPU_8916, "MSM8616"},

	/* 8x96 IDs */
	[246] = {MSM_CPU_8996, "MSM8996"},
	[310] = {MSM_CPU_8996, "MSM8996AU"},
	[311] = {MSM_CPU_8996, "APQ8096AU"},
	[291] = {MSM_CPU_8996, "APQ8096"},
	[305] = {MSM_CPU_8996, "MSM8996SG"},
	[312] = {MSM_CPU_8996, "APQ8096SG"},

	/*
	 * Uninitialized IDs are not known to run Linux.
	 * MSM_CPU_UNKNOWN is set to 0 to ensure these IDs are
	 * considered as unknown CPU.
	 */
};

static u32 socinfo_format;

static u32 socinfo_get_id(void);

/* socinfo: sysfs functions */

static char *socinfo_get_id_string(void)
{
	return (socinfo) ? cpu_of_id[socinfo->v0_1.id].soc_id_string : NULL;
}

static char *socinfo_get_image_version_base_address(struct device *dev)
{
	size_t size, size_in;
	void *ptr;

	ptr = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_IMAGE_VERSION_TABLE,
			&size);
	if (IS_ERR(ptr))
		return ptr;

	size_in = SMEM_IMAGE_VERSION_SIZE;
	if (size_in != size) {
		dev_err(dev, "Wrong size for smem item\n");
		return ERR_PTR(-EINVAL);
	}

	return ptr;
}


static u32 socinfo_get_version(void)
{
	return (socinfo) ? socinfo->v0_1.version : 0;
}

static char *socinfo_get_build_id(void)
{
	return (socinfo) ? socinfo->v0_1.build_id : NULL;
}

static u32 socinfo_get_raw_version(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_FORMAT(2) ?
			socinfo->v0_2.raw_version : 0)
		: 0;
}

static u32 socinfo_get_platform_type(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_FORMAT(3) ?
			socinfo->v0_3.hw_platform : 0)
		: 0;
}

static u32 socinfo_get_platform_version(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_FORMAT(4) ?
			socinfo->v0_4.platform_version : 0)
		: 0;
}

static u32 socinfo_get_platform_subtype(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_FORMAT(6) ?
			socinfo->v0_6.hw_platform_subtype : 0)
		: 0;
}

static u32 socinfo_get_serial_number(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_FORMAT(10) ?
			socinfo->v0_10.serial_number : 0)
		: 0;
}

static u32 socinfo_get_pmic_model(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_FORMAT(7) ?
			socinfo->v0_7.pmic_model : PMIC_MODEL_UNKNOWN)
		: PMIC_MODEL_UNKNOWN;
}

static u32 socinfo_get_pmic_die_revision(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_FORMAT(7) ?
			socinfo->v0_7.pmic_die_revision : 0)
		: 0;
}


static ssize_t
qcom_get_vendor(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	return sprintf(buf, "%s", "Qualcomm\n");
}

static ssize_t
qcom_get_raw_version(struct device *dev,
		     struct device_attribute *attr,
		     char *buf)
{
	return sprintf(buf, "%u\n",
		socinfo_get_raw_version());
}

static ssize_t
qcom_get_build_id(struct device *dev,
		   struct device_attribute *attr,
		   char *buf)
{
	return scnprintf(buf, SMEM_SOCINFO_BUILD_ID_LENGTH, "%s\n",
				socinfo_get_build_id());
}

static ssize_t
qcom_get_hw_platform(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	u32 hw_type;

	hw_type = socinfo_get_platform_type();

	return sprintf(buf, "%s\n", hw_platform[hw_type]);
}

static ssize_t
qcom_get_platform_version(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return sprintf(buf, "%u\n", socinfo_get_platform_version());
}

static ssize_t
qcom_get_accessory_chip(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	u32 acc_chip = socinfo ? (socinfo_format >= SOCINFO_FORMAT(5) ?
		socinfo->v0_5.accessory_chip : 0) : 0;
	return sprintf(buf, "%u\n", acc_chip);
}

static ssize_t
qcom_get_platform_subtype(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	if (socinfo_get_platform_type() == HW_PLATFORM_QRD) {
		return sprintf(buf, "%s\n",
			qrd_hw_platform_subtype[hw_subtype]);
	} else {
		return sprintf(buf, "%s\n",
			hw_platform_subtype[hw_subtype]);
	}
}

static ssize_t
qcom_get_platform_subtype_id(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%u\n", socinfo_get_platform_subtype());
}

static ssize_t
qcom_get_foundry_id(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	u32 foundry_id = socinfo ? (socinfo_format >= SOCINFO_FORMAT(9) ?
		socinfo->v0_9.foundry_id : 0) : 0;
	return sprintf(buf, "%u\n", foundry_id);
}

static ssize_t
qcom_get_serial_number(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%u\n", socinfo_get_serial_number());
}

static ssize_t
qcom_get_chip_family(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	u32 chip_family = socinfo ? (socinfo_format >= SOCINFO_FORMAT(12) ?
		socinfo->v0_12.chip_family : 0) : 0;
	return sprintf(buf, "0x%x\n", chip_family);
}

static ssize_t
qcom_get_raw_device_family(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	u32 raw_dev_fam = socinfo ? (socinfo_format >= SOCINFO_FORMAT(12) ?
		socinfo->v0_12.raw_device_family : 0) : 0;
	return sprintf(buf, "0x%x\n", raw_dev_fam);
}

static ssize_t
qcom_get_raw_device_number(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	u32 raw_dev_num = socinfo ? (socinfo_format >= SOCINFO_FORMAT(12) ?
		socinfo->v0_12.raw_device_number : 0) : 0;
	return sprintf(buf, "0x%x\n", raw_dev_num);
}

static ssize_t
qcom_get_pmic_model(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	u32 pmic_id = socinfo_get_pmic_model();

	return sprintf(buf, "%s\n", pmic_model[pmic_id]);
}

static ssize_t
qcom_get_pmic_die_revision(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	return sprintf(buf, "%u\n", socinfo_get_pmic_die_revision());
}

static ssize_t
qcom_get_image_version(int index, char *buf)
{
	return scnprintf(buf, SMEM_IMAGE_VERSION_NAME_SIZE, "%s\n",
			smem_image_version[index].name);
}

static ssize_t
qcom_set_image_version(int index, const char *buf)
{
	return strlcpy(smem_image_version[index].name, buf,
			SMEM_IMAGE_VERSION_NAME_SIZE);
}

static ssize_t
qcom_get_image_variant(int index, char *buf)
{
	return scnprintf(buf, SMEM_IMAGE_VERSION_VARIANT_SIZE, "%s\n",
			smem_image_version[index].variant);
}

static ssize_t
qcom_set_image_variant(int index, const char *buf)
{
	return strlcpy(smem_image_version[index].variant, buf,
			SMEM_IMAGE_VERSION_VARIANT_SIZE);
}

static ssize_t
qcom_get_image_crm_version(int index, char *buf)
{
	return scnprintf(buf, SMEM_IMAGE_VERSION_OEM_SIZE, "%s\n",
			smem_image_version[index].oem);
}

static ssize_t
qcom_set_image_crm_version(int index, const char *buf)
{
	return strlcpy(smem_image_version[index].oem, buf,
			SMEM_IMAGE_VERSION_OEM_SIZE);
}

static struct device_attribute qcom_soc_attr_raw_version =
	__ATTR(raw_version, 0444, qcom_get_raw_version,  NULL);

static struct device_attribute qcom_soc_attr_vendor =
	__ATTR(vendor, 0444, qcom_get_vendor,  NULL);

static struct device_attribute qcom_soc_attr_build_id =
	__ATTR(build_id, 0444, qcom_get_build_id, NULL);

static struct device_attribute qcom_soc_attr_hw_platform =
	__ATTR(hw_platform, 0444, qcom_get_hw_platform, NULL);


static struct device_attribute qcom_soc_attr_platform_version =
	__ATTR(platform_version, 0444,
			qcom_get_platform_version, NULL);

static struct device_attribute qcom_soc_attr_accessory_chip =
	__ATTR(accessory_chip, 0444,
			qcom_get_accessory_chip, NULL);

static struct device_attribute qcom_soc_attr_platform_subtype =
	__ATTR(platform_subtype, 0444,
			qcom_get_platform_subtype, NULL);

static struct device_attribute qcom_soc_attr_platform_subtype_id =
	__ATTR(platform_subtype_id, 0444,
			qcom_get_platform_subtype_id, NULL);

static struct device_attribute qcom_soc_attr_foundry_id =
	__ATTR(foundry_id, 0444,
			qcom_get_foundry_id, NULL);

static struct device_attribute qcom_soc_attr_serial_number =
	__ATTR(serial_number, 0444,
			qcom_get_serial_number, NULL);

static struct device_attribute qcom_soc_attr_chip_family =
	__ATTR(chip_family, 0444,
			qcom_get_chip_family, NULL);

static struct device_attribute qcom_soc_attr_raw_device_family =
	__ATTR(raw_device_family, 0444,
			qcom_get_raw_device_family, NULL);

static struct device_attribute qcom_soc_attr_raw_device_number =
	__ATTR(raw_device_number, 0444,
			qcom_get_raw_device_number, NULL);

static struct device_attribute qcom_soc_attr_pmic_model =
	__ATTR(pmic_model, 0444,
			qcom_get_pmic_model, NULL);

static struct device_attribute qcom_soc_attr_pmic_die_revision =
	__ATTR(pmic_die_revision, 0444,
			qcom_get_pmic_die_revision, NULL);

SMEM_IMAGE_TABLE_RO_ATTR(boot, SMEM_IMAGE_TABLE_BOOT_INDEX);
SMEM_IMAGE_TABLE_RO_ATTR(tz, SMEM_IMAGE_TABLE_TZ_INDEX);
SMEM_IMAGE_TABLE_RO_ATTR(rpm, SMEM_IMAGE_TABLE_RPM_INDEX);
SMEM_IMAGE_TABLE_ATTR(apps, SMEM_IMAGE_TABLE_APPS_INDEX);
SMEM_IMAGE_TABLE_RO_ATTR(mpss, SMEM_IMAGE_TABLE_MPSS_INDEX);
SMEM_IMAGE_TABLE_RO_ATTR(adsp, SMEM_IMAGE_TABLE_ADSP_INDEX);
SMEM_IMAGE_TABLE_RO_ATTR(cnss, SMEM_IMAGE_TABLE_CNSS_INDEX);
SMEM_IMAGE_TABLE_RO_ATTR(video, SMEM_IMAGE_TABLE_VIDEO_INDEX);

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

static int socinfo_populate_sysfs_files(struct device *qcom_soc_device)
{
	int err, img_idx;

	/*
	 * Expose SMEM_IMAGE_TABLE to sysfs only when we have IMAGE_TABLE
	 * available in SMEM. As IMAGE_TABLE and SOCINFO are two separate
	 * items within SMEM, we expose the remaining soc information(i.e
	 * only the SOCINFO item available in SMEM) to sysfs even in the
	 * absence of an IMAGE_TABLE.
	 */
	if (smem_img_tbl_avlbl) {
		for (img_idx = 0; img_idx < SMEM_IMAGE_VERSION_BLOCKS_COUNT;
				img_idx++) {
			if (smem_image_table[img_idx]) {
				err = sysfs_create_group(&qcom_soc_device->kobj,
					smem_image_table[img_idx]);
				if (err)
					break;
			}
		}
	}
	err = device_create_file(qcom_soc_device, &qcom_soc_attr_vendor);
	if (!err) {
		switch (socinfo_format) {
		case SOCINFO_FORMAT(12):
			err = device_create_file(qcom_soc_device,
					&qcom_soc_attr_chip_family);
			if (!err)
				err = device_create_file(qcom_soc_device,
					&qcom_soc_attr_raw_device_family);
			if (!err)
				err = device_create_file(qcom_soc_device,
					&qcom_soc_attr_raw_device_number);
		case SOCINFO_FORMAT(11):
		case SOCINFO_FORMAT(10):
			if (!err)
				err = device_create_file(qcom_soc_device,
					&qcom_soc_attr_serial_number);
		case SOCINFO_FORMAT(9):
			if (!err)
				err = device_create_file(qcom_soc_device,
					&qcom_soc_attr_foundry_id);
		case SOCINFO_FORMAT(8):
		case SOCINFO_FORMAT(7):
			if (!err)
				err = device_create_file(qcom_soc_device,
					&qcom_soc_attr_pmic_model);
			if (!err)
				err = device_create_file(qcom_soc_device,
					&qcom_soc_attr_pmic_die_revision);
		case SOCINFO_FORMAT(6):
			if (hw_subtype_valid)
				if (!err)
					err = device_create_file(
						qcom_soc_device,
					&qcom_soc_attr_platform_subtype);
			if (!err)
				err = device_create_file(qcom_soc_device,
				&qcom_soc_attr_platform_subtype_id);
		case SOCINFO_FORMAT(5):
			if (!err)
				err = device_create_file(qcom_soc_device,
					&qcom_soc_attr_accessory_chip);
		case SOCINFO_FORMAT(4):
			if (!err)
				err = device_create_file(qcom_soc_device,
					&qcom_soc_attr_platform_version);
		case SOCINFO_FORMAT(3):
			if (!err)
				err = device_create_file(qcom_soc_device,
					&qcom_soc_attr_hw_platform);
		case SOCINFO_FORMAT(2):
			if (!err)
				err = device_create_file(qcom_soc_device,
						&qcom_soc_attr_raw_version);
		case SOCINFO_FORMAT(1):
			if (!err)
				err = device_create_file(qcom_soc_device,
						&qcom_soc_attr_build_id);
			if (err) {
				dev_err(qcom_soc_device,
					"Could not create sysfs entry\n");
				return err;
			}
			return 0;
		default:
			dev_err(qcom_soc_device, "Unknown socinfo format: v0.%u\n",
					SOCINFO_VERSION_MINOR(socinfo_format));
			return -EINVAL;
		}
	} else {
		dev_err(qcom_soc_device, "Could not create sysfs entry\n");
		return err;
	}
}

static int socinfo_init_sysfs(struct device *dev)
{
	struct device *qcom_soc_device;
	struct soc_device *soc_dev;
	struct soc_device_attribute *soc_dev_attr;
	u32 soc_version;

	soc_dev_attr = kzalloc(sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr)
		return -ENOMEM;

	soc_version = socinfo_get_version();

	soc_dev_attr->soc_id = kasprintf(GFP_KERNEL, "%d", socinfo_get_id());
	soc_dev_attr->family = "Snapdragon";
	soc_dev_attr->machine = socinfo_get_id_string();
	soc_dev_attr->revision = kasprintf(GFP_KERNEL, "%u.%u",
				SOC_VERSION_MAJOR(soc_version),
				SOC_VERSION_MINOR(soc_version));

	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR(soc_dev)) {
		kfree(soc_dev_attr);
		return PTR_ERR(soc_dev);
	}

	qcom_soc_device = soc_device_to_device(soc_dev);
	socinfo_populate_sysfs_files(qcom_soc_device);
	return 0;
}

static u32 socinfo_get_id(void)
{
	return (socinfo) ? socinfo->v0_1.id : 0;
}

static void socinfo_print(struct device *dev)
{
	u32 f_min = SOCINFO_VERSION_MINOR(socinfo_format);
	u32 v_maj = SOC_VERSION_MAJOR(socinfo->v0_1.version);
	u32 v_min = SOC_VERSION_MINOR(socinfo->v0_1.version);

	switch (socinfo_format) {
	case SOCINFO_FORMAT(1):
		dev_info(dev, "socinfo: v0.%u, id=%u, ver=%u.%u\n",
			f_min, socinfo->v0_1.id, v_maj, v_min);
		break;
	case SOCINFO_FORMAT(2):
		dev_info(dev, "socinfo: v0.%u, id=%u, ver=%u.%u, raw_ver=%u\n",
			f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_version);
		break;
	case SOCINFO_FORMAT(3):
		dev_info(dev, "socinfo: v0.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u\n",
			f_min, socinfo->v0_1.id,
			v_maj, v_min, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform);
		break;
	case SOCINFO_FORMAT(4):
		dev_info(dev, "socinfo: v0.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n",
			f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version);
		break;
	case SOCINFO_FORMAT(5):
		dev_info(dev, "socinfo: v0.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u accessory_chip=%u\n",
			f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip);
		break;
	case SOCINFO_FORMAT(6):
		dev_info(dev, "socinfo: v0.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u accessory_chip=%u, hw_plat_subtype=%u\n",
			f_min, socinfo->v0_1.id,
			v_maj, v_min, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype);
		break;
	case SOCINFO_FORMAT(7):
	case SOCINFO_FORMAT(8):
		dev_info(dev, "socinfo: v0.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u\n",
			f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision);
		break;
	case SOCINFO_FORMAT(9):
		dev_info(dev, "socinfo: v0.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u foundry_id=%u\n",
			f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id);
		break;
	case SOCINFO_FORMAT(10):
		dev_info(dev, "socinfo: v0.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u, foundry_id=%u, serial_number=%u\n",
			f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id,
			socinfo->v0_10.serial_number);
		break;
	case SOCINFO_FORMAT(11):
		dev_info(dev, "socinfo: v0.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u, accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u, foundry_id=%u, serial_number=%u num_pmics=%u\n",
			f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id,
			socinfo->v0_10.serial_number,
			socinfo->v0_11.num_pmics);
		break;
	case SOCINFO_FORMAT(12):
		dev_info(dev, "socinfo: v0.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u, foundry_id=%u, serial_number=%u, num_pmics=%u, chip_family=0x%x, raw_device_family=0x%x, raw_device_number=0x%x\n",
			f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id,
			socinfo->v0_10.serial_number,
			socinfo->v0_11.num_pmics,
			socinfo->v0_12.chip_family,
			socinfo->v0_12.raw_device_family,
			socinfo->v0_12.raw_device_number);
		break;

	default:
		dev_err(dev, "socinfo: Unknown format found: v0.%u\n", f_min);
		break;
	}
}

static int socinfo_select_format(struct device *dev)
{
	u32 f_maj = SOCINFO_VERSION_MAJOR(socinfo->v0_1.format);
	u32 f_min = SOCINFO_VERSION_MINOR(socinfo->v0_1.format);

	if (f_maj != 0) {
		dev_err(dev, "Unsupported format v%u.%u.\n",
			f_maj, f_min);
		return -EINVAL;
	}

	if (socinfo->v0_1.format > MAX_SOCINFO_FORMAT) {
		dev_warn(dev, "Unsupported format v%u.%u. Falling back to v%u.%u.\n",
			f_maj, f_min, SOCINFO_VERSION_MAJOR(MAX_SOCINFO_FORMAT),
			SOCINFO_VERSION_MINOR(MAX_SOCINFO_FORMAT));
		socinfo_format = MAX_SOCINFO_FORMAT;
	} else {
		socinfo_format = socinfo->v0_1.format;
	}
	return 0;
}

int qcom_socinfo_init(struct platform_device *pdev)
{
	size_t size;

	socinfo = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_HW_SW_BUILD_ID,
			&size);
	if (IS_ERR(socinfo) || (socinfo_select_format(&pdev->dev) < 0)) {
		dev_warn(&pdev->dev,
			"Coudn't find soc information; Unable to setup socinfo.\n");
		return -ENOMEM;
	}

	if (!socinfo_get_id()) {
		dev_err(&pdev->dev, "socinfo: Unknown SoC ID!\n");
		return -EINVAL;
	}

	WARN(socinfo_get_id() >= ARRAY_SIZE(cpu_of_id),
		"New IDs added! ID => CPU mapping needs an update.\n");

	socinfo_print(&pdev->dev);

	smem_image_version = (struct smem_image_version *)
			socinfo_get_image_version_base_address(&pdev->dev);
	if (IS_ERR(smem_image_version)) {
		dev_warn(&pdev->dev, "Unable to get address for image version table.\n");
		smem_img_tbl_avlbl = false;
	}

	hw_subtype = socinfo_get_platform_subtype();
	if (socinfo_get_platform_type() == HW_PLATFORM_QRD) {
		if (hw_subtype >= PLATFORM_SUBTYPE_QRD_INVALID) {
			dev_err(&pdev->dev, "Invalid hardware platform sub type for qrd found\n");
			hw_subtype_valid = false;
		}
	} else {
		if (hw_subtype >= PLATFORM_SUBTYPE_INVALID) {
			dev_err(&pdev->dev, "Invalid hardware platform subtype\n");
			hw_subtype_valid = false;
		}
	}

	socinfo_init_sysfs(&pdev->dev);

	/* Feed the soc specific unique data into entropy pool */
	add_device_randomness(socinfo, size);

	return 0;
}
EXPORT_SYMBOL(qcom_socinfo_init);

