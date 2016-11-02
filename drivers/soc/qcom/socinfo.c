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

/*
 * SOC Info Routines
 *
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

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

/*
 * SOC version type with major number in the upper 16 bits and minor
 * number in the lower 16 bits.  For example:
 *   1.0 -> 0x00010000
 *   2.3 -> 0x00020003
 */
#define SOCINFO_VERSION_MAJOR(ver) (((ver) & 0xffff0000) >> 16)
#define SOCINFO_VERSION_MINOR(ver) ((ver) & 0x0000ffff)
#define SOCINFO_VERSION(maj, min)  ((((maj) & 0xffff) << 16)|((min) & 0xffff))

#define PLATFORM_SUBTYPE_MDM	1
#define PLATFORM_SUBTYPE_INTERPOSERV3 2
#define PLATFORM_SUBTYPE_SGLTE	6

#define SMEM_SOCINFO_BUILD_ID_LENGTH 32
#define SMEM_IMAGE_VERSION_BLOCKS_COUNT 32
#define SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE 128
#define SMEM_IMAGE_VERSION_SIZE 4096
#define SMEM_IMAGE_VERSION_NAME_SIZE 75
#define SMEM_IMAGE_VERSION_VARIANT_SIZE 20
#define SMEM_IMAGE_VERSION_VARIANT_OFFSET 75
#define SMEM_IMAGE_VERSION_OEM_SIZE 32
#define SMEM_IMAGE_VERSION_OEM_OFFSET 96
#define SMEM_IMAGE_VERSION_PARTITION_APPS 10
#define SMEM_ITEM_SIZE_ALIGN 8

/*
 * Shared memory identifiers, used to acquire handles to respective memory
 * region.
 */
#define SMEM_IMAGE_VERSION_TABLE	469

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

/*
 * Qcom SoC IDs
 */
enum qcom_cpu_id {
	MSM_UNKNOWN_ID,
	MSM_8960_ID = 87,
	APQ_8064_ID = 109,
	MSM_8660A_ID = 122,
	MSM_8260A_ID,
	APQ_8060A_ID,
	MSM_8974_ID = 126,
	MPQ_8064_ID = 130,
	MSM_8960AB_ID = 138,
	APQ_8060AB_ID,
	MSM_8260AB_ID,
	MSM_8660AB_ID,
	APQ_8084_ID = 178,
	APQ_8074_ID = 184,
	MSM_8274_ID,
	MSM_8674_ID,
	MSM_8974PRO_ID = 194,
	MSM_8916_ID = 206,
	APQ_8074_AA_ID = 208,
	APQ_8074_AB_ID,
	APQ_8074PRO_ID,
	MSM_8274_AA_ID,
	MSM_8274_AB_ID,
	MSM_8274PRO_ID,
	MSM_8674_AA_ID,
	MSM_8674_AB_ID,
	MSM_8674PRO_ID,
	MSM_8974_AA_ID,
	MSM_8974_AB_ID,
	MSM_8996_ID = 246,
	APQ_8016_ID,
	MSM_8216_ID,
	MSM_8116_ID,
	MSM_8616_ID,
	APQ8096_ID = 291,
	MSM_8996SG_ID = 305,
	MSM_8996AU_ID = 310,
	APQ_8096AU_ID,
	APQ_8096SG_ID
};

struct qcom_soc_info {
	enum qcom_cpu generic_soc_type;
	char *soc_id_string;
};

enum qcom_pmic_model {
	PMIC_MODEL_PM8058 = 13,
	PMIC_MODEL_PM8028,
	PMIC_MODEL_PM8901,
	PMIC_MODEL_PM8027,
	PMIC_MODEL_ISL_9519,
	PMIC_MODEL_PM8921,
	PMIC_MODEL_PM8018,
	PMIC_MODEL_PM8015,
	PMIC_MODEL_PM8014,
	PMIC_MODEL_PM8821,
	PMIC_MODEL_PM8038,
	PMIC_MODEL_PM8922,
	PMIC_MODEL_PM8917,
	PMIC_MODEL_UNKNOWN = 0xFFFFFFFF
};

enum hw_platform_type {
	HW_PLATFORM_UNKNOWN,
	HW_PLATFORM_SURF,
	HW_PLATFORM_FFA,
	HW_PLATFORM_FLUID,
	HW_PLATFORM_SVLTE_FFA,
	HW_PLATFORM_SVLTE_SURF,
	HW_PLATFORM_MTP_MDM = 7,
	HW_PLATFORM_MTP,
	HW_PLATFORM_LIQUID,
	/* Dragonboard platform id is assigned as 10 in CDT */
	HW_PLATFORM_DRAGON,
	HW_PLATFORM_QRD,
	HW_PLATFORM_HRD	= 13,
	HW_PLATFORM_DTV,
	HW_PLATFORM_RCM	= 21,
	HW_PLATFORM_STP = 23,
	HW_PLATFORM_SBC,
	HW_PLATFORM_INVALID
};

enum accessory_chip_type {
	ACCESSORY_CHIP_UNKNOWN = 0,
	ACCESSORY_CHIP_CHARM = 58
};

enum qrd_platform_subtype {
	PLATFORM_SUBTYPE_QRD,
	PLATFORM_SUBTYPE_SKUAA,
	PLATFORM_SUBTYPE_SKUF,
	PLATFORM_SUBTYPE_SKUAB,
	PLATFORM_SUBTYPE_SKUG = 0x5,
	PLATFORM_SUBTYPE_QRD_INVALID
};

enum platform_subtype {
	PLATFORM_SUBTYPE_UNKNOWN,
	PLATFORM_SUBTYPE_CHARM,
	PLATFORM_SUBTYPE_STRANGE,
	PLATFORM_SUBTYPE_STRANGE_2A,
	PLATFORM_SUBTYPE_INVALID
};

#ifdef CONFIG_SOC_BUS
static const char *hw_platform[] = {
	[HW_PLATFORM_UNKNOWN] = "Unknown",
	[HW_PLATFORM_SURF] = "Surf",
	[HW_PLATFORM_FFA] = "FFA",
	[HW_PLATFORM_FLUID] = "Fluid",
	[HW_PLATFORM_SVLTE_FFA] = "SVLTE_FFA",
	[HW_PLATFORM_SVLTE_SURF] = "SLVTE_SURF",
	[HW_PLATFORM_MTP_MDM] = "MDM_MTP_NO_DISPLAY",
	[HW_PLATFORM_MTP] = "MTP",
	[HW_PLATFORM_RCM] = "RCM",
	[HW_PLATFORM_LIQUID] = "Liquid",
	[HW_PLATFORM_DRAGON] = "Dragon",
	[HW_PLATFORM_QRD] = "QRD",
	[HW_PLATFORM_HRD] = "HRD",
	[HW_PLATFORM_DTV] = "DTV",
	[HW_PLATFORM_STP] = "STP",
	[HW_PLATFORM_SBC] = "SBC",
};

static const char *qrd_hw_platform_subtype[] = {
	[PLATFORM_SUBTYPE_QRD] = "QRD",
	[PLATFORM_SUBTYPE_SKUAA] = "SKUAA",
	[PLATFORM_SUBTYPE_SKUF] = "SKUF",
	[PLATFORM_SUBTYPE_SKUAB] = "SKUAB",
	[PLATFORM_SUBTYPE_SKUG] = "SKUG",
	[PLATFORM_SUBTYPE_QRD_INVALID] = "INVALID",
};

static const char *hw_platform_subtype[] = {
	[PLATFORM_SUBTYPE_UNKNOWN] = "Unknown",
	[PLATFORM_SUBTYPE_CHARM] = "charm",
	[PLATFORM_SUBTYPE_STRANGE] = "strange",
	[PLATFORM_SUBTYPE_STRANGE_2A] = "strange_2a",
	[PLATFORM_SUBTYPE_INVALID] = "Invalid",
};
#endif

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

/* max socinfo format version supported */
#define MAX_SOCINFO_FORMAT SOCINFO_VERSION(0, 12)

static struct qcom_soc_info cpu_of_id[] = {

	[MSM_UNKNOWN_ID] = {MSM_CPU_UNKNOWN, "Unknown CPU"},

	/* 8x60 IDs */
	[MSM_8960_ID] = {MSM_CPU_8960, "MSM8960"},

	/* 8x64 IDs */
	[APQ_8064_ID] = {MSM_CPU_8064, "APQ8064"},
	[MPQ_8064_ID] = {MSM_CPU_8064, "MPQ8064"},

	/* 8x60A IDs */
	[MSM_8660A_ID] = {MSM_CPU_8960, "MSM8660A"},
	[MSM_8260A_ID] = {MSM_CPU_8960, "MSM8260A"},
	[APQ_8060A_ID] = {MSM_CPU_8960, "APQ8060A"},

	/* 8x74 IDs */
	[MSM_8974_ID] = {MSM_CPU_8974, "MSM8974"},
	[APQ_8074_ID] = {MSM_CPU_8974, "APQ8074"},
	[MSM_8274_ID] = {MSM_CPU_8974, "MSM8274"},
	[MSM_8674_ID] = {MSM_CPU_8974, "MSM8674"},

	/* 8x74AA IDs */
	[APQ_8074_AA_ID] = {MSM_CPU_8974PRO_AA, "APQ8074-AA"},
	[MSM_8274_AA_ID] = {MSM_CPU_8974PRO_AA, "MSM8274-AA"},
	[MSM_8674_AA_ID] = {MSM_CPU_8974PRO_AA, "MSM8674-AA"},
	[MSM_8974_AA_ID] = {MSM_CPU_8974PRO_AA, "MSM8974-AA"},

	/* 8x74AB IDs */
	[APQ_8074_AB_ID] = {MSM_CPU_8974PRO_AB, "APQ8074-AB"},
	[MSM_8274_AB_ID] = {MSM_CPU_8974PRO_AB, "MSM8274-AB"},
	[MSM_8674_AB_ID] = {MSM_CPU_8974PRO_AB, "MSM8674-AB"},
	[MSM_8974_AB_ID] = {MSM_CPU_8974PRO_AB, "MSM8974-AB"},

	/* 8x74AC IDs */
	[MSM_8974PRO_ID] = {MSM_CPU_8974PRO_AC, "MSM8974PRO"},
	[APQ_8074PRO_ID] = {MSM_CPU_8974PRO_AC, "APQ8074PRO"},
	[MSM_8274PRO_ID] = {MSM_CPU_8974PRO_AC, "MSM8274PRO"},
	[MSM_8674PRO_ID] = {MSM_CPU_8974PRO_AC, "MSM8674PRO"},

	/* 8x60AB IDs */
	[MSM_8960AB_ID] = {MSM_CPU_8960AB, "MSM8960AB"},
	[APQ_8060AB_ID] = {MSM_CPU_8960AB, "APQ8060AB"},
	[MSM_8260AB_ID] = {MSM_CPU_8960AB, "MSM8260AB"},
	[MSM_8660AB_ID] = {MSM_CPU_8960AB, "MSM8660AB"},

	/* 8x84 IDs */
	[APQ_8084_ID] = {MSM_CPU_8084, "APQ8084"},

	/* 8x16 IDs */
	[MSM_8916_ID] = {MSM_CPU_8916, "MSM8916"},
	[APQ_8016_ID] = {MSM_CPU_8916, "APQ8016"},
	[MSM_8216_ID] = {MSM_CPU_8916, "MSM8216"},
	[MSM_8116_ID] = {MSM_CPU_8916, "MSM8116"},
	[MSM_8616_ID] = {MSM_CPU_8916, "MSM8616"},

	/* 8x96 IDs */
	[MSM_8996_ID] = {MSM_CPU_8996, "MSM8996"},
	[MSM_8996AU_ID] = {MSM_CPU_8996, "MSM8996AU"},
	[APQ_8096AU_ID] = {MSM_CPU_8996, "APQ8096AU"},
	[APQ8096_ID] = {MSM_CPU_8996, "APQ8096"},
	[MSM_8996SG_ID] = {MSM_CPU_8996, "MSM8996SG"},
	[APQ_8096SG_ID] = {MSM_CPU_8996, "APQ8096SG"},

	/*
	 * Uninitialized IDs are not known to run Linux.
	 * MSM_CPU_UNKNOWN is set to 0 to ensure these IDs are
	 * considered as unknown CPU.
	 */
};

static u32 socinfo_format;

static struct socinfo_v0_1 dummy_socinfo = {
	.format = SOCINFO_VERSION(0, 1),
	.version = 1,
};

#ifdef CONFIG_SOC_BUS
static int current_image;
static u32 socinfo_get_id(void);

/* socinfo: sysfs functions */

static char *socinfo_get_id_string(void)
{
	return (socinfo) ? cpu_of_id[socinfo->v0_1.id].soc_id_string : NULL;
}

static u32 socinfo_get_accessory_chip(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 5) ?
			socinfo->v0_5.accessory_chip : 0)
		: 0;
}

static u32 socinfo_get_foundry_id(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 9) ?
			socinfo->v0_9.foundry_id : 0)
		: 0;
}

static u32 socinfo_get_chip_family(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 12) ?
			socinfo->v0_12.chip_family : 0)
		: 0;
}

static u32 socinfo_get_raw_device_family(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 12) ?
			socinfo->v0_12.raw_device_family : 0)
		: 0;
}

static u32 socinfo_get_raw_device_number(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 12) ?
			socinfo->v0_12.raw_device_number : 0)
		: 0;
}

static char *socinfo_get_image_version_base_address(struct device *dev)
{
	size_t size, size_in;
	void *ptr;

	ptr = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_IMAGE_VERSION_TABLE,
			&size);
	if (IS_ERR(ptr))
		return ptr;

	size_in = ALIGN(SMEM_IMAGE_VERSION_SIZE, SMEM_ITEM_SIZE_ALIGN);
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
		(socinfo_format >= SOCINFO_VERSION(0, 2) ?
			socinfo->v0_2.raw_version : 0)
		: 0;
}

static u32 socinfo_get_platform_type(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 3) ?
			socinfo->v0_3.hw_platform : 0)
		: 0;
}

static u32 socinfo_get_platform_version(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 4) ?
			socinfo->v0_4.platform_version : 0)
		: 0;
}

static u32 socinfo_get_platform_subtype(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 6) ?
			socinfo->v0_6.hw_platform_subtype : 0)
		: 0;
}

static u32 socinfo_get_serial_number(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 10) ?
			socinfo->v0_10.serial_number : 0)
		: 0;
}

static enum qcom_pmic_model socinfo_get_pmic_model(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 7) ?
			socinfo->v0_7.pmic_model : PMIC_MODEL_UNKNOWN)
		: PMIC_MODEL_UNKNOWN;
}

static u32 socinfo_get_pmic_die_revision(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 7) ?
			socinfo->v0_7.pmic_die_revision : 0)
		: 0;
}


static ssize_t
qcom_get_vendor(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	return snprintf(buf, PAGE_SIZE, "Qualcomm\n");
}

static ssize_t
qcom_get_raw_version(struct device *dev,
		     struct device_attribute *attr,
		     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_raw_version());
}

static ssize_t
qcom_get_build_id(struct device *dev,
		   struct device_attribute *attr,
		   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%-.32s\n",
			socinfo_get_build_id());
}

static ssize_t
qcom_get_hw_platform(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	u32 hw_type;

	hw_type = socinfo_get_platform_type();

	return snprintf(buf, PAGE_SIZE, "%-.32s\n",
			hw_platform[hw_type]);
}

static ssize_t
qcom_get_platform_version(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_platform_version());
}

static ssize_t
qcom_get_accessory_chip(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_accessory_chip());
}

static ssize_t
qcom_get_platform_subtype(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	u32 hw_subtype;

	hw_subtype = socinfo_get_platform_subtype();
	if (socinfo_get_platform_type() == HW_PLATFORM_QRD) {
		if (hw_subtype >= PLATFORM_SUBTYPE_QRD_INVALID) {
			pr_err("Invalid hardware platform sub type for qrd found\n");
			hw_subtype = PLATFORM_SUBTYPE_QRD_INVALID;
		}
		return snprintf(buf, PAGE_SIZE, "%-.32s\n",
					qrd_hw_platform_subtype[hw_subtype]);
	} else {
		if (hw_subtype >= PLATFORM_SUBTYPE_INVALID) {
			pr_err("Invalid hardware platform subtype\n");
			hw_subtype = PLATFORM_SUBTYPE_INVALID;
		}
		return snprintf(buf, PAGE_SIZE, "%-.32s\n",
			hw_platform_subtype[hw_subtype]);
	}
}

static ssize_t
qcom_get_platform_subtype_id(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	u32 hw_subtype;

	hw_subtype = socinfo_get_platform_subtype();
	return snprintf(buf, PAGE_SIZE, "%u\n",
		hw_subtype);
}

static ssize_t
qcom_get_foundry_id(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_foundry_id());
}

static ssize_t
qcom_get_serial_number(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_serial_number());
}

static ssize_t
qcom_get_chip_family(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%x\n",
		socinfo_get_chip_family());
}

static ssize_t
qcom_get_raw_device_family(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%x\n",
		socinfo_get_raw_device_family());
}

static ssize_t
qcom_get_raw_device_number(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%x\n",
		socinfo_get_raw_device_number());
}

static ssize_t
qcom_get_pmic_model(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_pmic_model());
}

static ssize_t
qcom_get_pmic_die_revision(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
			 socinfo_get_pmic_die_revision());
}

static ssize_t
qcom_get_image_version(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	char *string_address;

	string_address = socinfo_get_image_version_base_address(dev);
	if (IS_ERR(string_address)) {
		pr_err("Failed to get image version base address");
		return snprintf(buf, SMEM_IMAGE_VERSION_NAME_SIZE, "Unknown");
	}
	string_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	return snprintf(buf, SMEM_IMAGE_VERSION_NAME_SIZE, "%-.75s\n",
			string_address);
}

static ssize_t
qcom_set_image_version(struct device *dev,
			struct device_attribute *attr,
			const char *buf,
			size_t count)
{
	char *store_address;

	if (current_image != SMEM_IMAGE_VERSION_PARTITION_APPS)
		return count;
	store_address = socinfo_get_image_version_base_address(dev);
	if (IS_ERR(store_address)) {
		pr_err("Failed to get image version base address");
		return count;
	}
	store_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	snprintf(store_address, SMEM_IMAGE_VERSION_NAME_SIZE, "%-.75s", buf);
	return count;
}

static ssize_t
qcom_get_image_variant(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	char *string_address;

	string_address = socinfo_get_image_version_base_address(dev);
	if (IS_ERR(string_address)) {
		pr_err("Failed to get image version base address");
		return snprintf(buf, SMEM_IMAGE_VERSION_VARIANT_SIZE,
		"Unknown");
	}
	string_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	string_address += SMEM_IMAGE_VERSION_VARIANT_OFFSET;
	return snprintf(buf, SMEM_IMAGE_VERSION_VARIANT_SIZE, "%-.20s\n",
			string_address);
}

static ssize_t
qcom_set_image_variant(struct device *dev,
			struct device_attribute *attr,
			const char *buf,
			size_t count)
{
	char *store_address;

	if (current_image != SMEM_IMAGE_VERSION_PARTITION_APPS)
		return count;
	store_address = socinfo_get_image_version_base_address(dev);
	if (IS_ERR(store_address)) {
		pr_err("Failed to get image version base address");
		return count;
	}
	store_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	store_address += SMEM_IMAGE_VERSION_VARIANT_OFFSET;
	snprintf(store_address, SMEM_IMAGE_VERSION_VARIANT_SIZE, "%-.20s", buf);
	return count;
}

static ssize_t
qcom_get_image_crm_version(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	char *string_address;

	string_address = socinfo_get_image_version_base_address(dev);
	if (IS_ERR(string_address)) {
		pr_err("Failed to get image version base address");
		return snprintf(buf, SMEM_IMAGE_VERSION_OEM_SIZE, "Unknown");
	}
	string_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	string_address += SMEM_IMAGE_VERSION_OEM_OFFSET;
	return snprintf(buf, SMEM_IMAGE_VERSION_OEM_SIZE, "%-.32s\n",
			string_address);
}

static ssize_t
qcom_set_image_crm_version(struct device *dev,
			struct device_attribute *attr,
			const char *buf,
			size_t count)
{
	char *store_address;

	if (current_image != SMEM_IMAGE_VERSION_PARTITION_APPS)
		return count;
	store_address = socinfo_get_image_version_base_address(dev);
	if (IS_ERR(store_address)) {
		pr_err("Failed to get image version base address");
		return count;
	}
	store_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	store_address += SMEM_IMAGE_VERSION_OEM_OFFSET;
	snprintf(store_address, SMEM_IMAGE_VERSION_OEM_SIZE, "%-.32s", buf);
	return count;
}

static ssize_t
qcom_get_image_number(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n",
			current_image);
}

static ssize_t
qcom_select_image(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	int ret, digit;

	ret = kstrtoint(buf, 10, &digit);
	if (ret)
		return ret;
	if (digit >= 0 && digit < SMEM_IMAGE_VERSION_BLOCKS_COUNT)
		current_image = digit;
	else
		current_image = 0;
	return count;
}

static ssize_t
qcom_get_images(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int pos = 0;
	int image;
	char *image_address;

	image_address = socinfo_get_image_version_base_address(dev);
	if (IS_ERR(image_address))
		return snprintf(buf, PAGE_SIZE, "Unavailable\n");

	*buf = '\0';
	for (image = 0; image < SMEM_IMAGE_VERSION_BLOCKS_COUNT; image++) {
		if (*image_address == '\0') {
			image_address += SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
			continue;
		}

		pos += snprintf(buf + pos, PAGE_SIZE - pos, "%d:\n",
				image);
		pos += snprintf(buf + pos, PAGE_SIZE - pos,
				"\tCRM:\t\t%-.75s\n", image_address);
		pos += snprintf(buf + pos, PAGE_SIZE - pos,
				"\tVariant:\t%-.20s\n", image_address +
				SMEM_IMAGE_VERSION_VARIANT_OFFSET);
		pos += snprintf(buf + pos, PAGE_SIZE - pos,
				"\tVersion:\t%-.32s\n\n",
				image_address + SMEM_IMAGE_VERSION_OEM_OFFSET);

		image_address += SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	}

	return pos;
}

static struct device_attribute qcom_soc_attr_raw_version =
	__ATTR(raw_version, S_IRUGO, qcom_get_raw_version,  NULL);

static struct device_attribute qcom_soc_attr_vendor =
	__ATTR(vendor, S_IRUGO, qcom_get_vendor,  NULL);

static struct device_attribute qcom_soc_attr_build_id =
	__ATTR(build_id, S_IRUGO, qcom_get_build_id, NULL);

static struct device_attribute qcom_soc_attr_hw_platform =
	__ATTR(hw_platform, S_IRUGO, qcom_get_hw_platform, NULL);


static struct device_attribute qcom_soc_attr_platform_version =
	__ATTR(platform_version, S_IRUGO,
			qcom_get_platform_version, NULL);

static struct device_attribute qcom_soc_attr_accessory_chip =
	__ATTR(accessory_chip, S_IRUGO,
			qcom_get_accessory_chip, NULL);

static struct device_attribute qcom_soc_attr_platform_subtype =
	__ATTR(platform_subtype, S_IRUGO,
			qcom_get_platform_subtype, NULL);

static struct device_attribute qcom_soc_attr_platform_subtype_id =
	__ATTR(platform_subtype_id, S_IRUGO,
			qcom_get_platform_subtype_id, NULL);

static struct device_attribute qcom_soc_attr_foundry_id =
	__ATTR(foundry_id, S_IRUGO,
			qcom_get_foundry_id, NULL);

static struct device_attribute qcom_soc_attr_serial_number =
	__ATTR(serial_number, S_IRUGO,
			qcom_get_serial_number, NULL);

static struct device_attribute qcom_soc_attr_chip_family =
	__ATTR(chip_family, S_IRUGO,
			qcom_get_chip_family, NULL);

static struct device_attribute qcom_soc_attr_raw_device_family =
	__ATTR(raw_device_family, S_IRUGO,
			qcom_get_raw_device_family, NULL);

static struct device_attribute qcom_soc_attr_raw_device_number =
	__ATTR(raw_device_number, S_IRUGO,
			qcom_get_raw_device_number, NULL);

static struct device_attribute qcom_soc_attr_pmic_model =
	__ATTR(pmic_model, S_IRUGO,
			qcom_get_pmic_model, NULL);

static struct device_attribute qcom_soc_attr_pmic_die_revision =
	__ATTR(pmic_die_revision, S_IRUGO,
			qcom_get_pmic_die_revision, NULL);

static struct device_attribute image_version =
	__ATTR(image_version, S_IRUGO | S_IWUSR,
			qcom_get_image_version, qcom_set_image_version);

static struct device_attribute image_variant =
	__ATTR(image_variant, S_IRUGO | S_IWUSR,
			qcom_get_image_variant, qcom_set_image_variant);

static struct device_attribute image_crm_version =
	__ATTR(image_crm_version, S_IRUGO | S_IWUSR,
			qcom_get_image_crm_version, qcom_set_image_crm_version);

static struct device_attribute select_image =
	__ATTR(select_image, S_IRUGO | S_IWUSR,
			qcom_get_image_number, qcom_select_image);

static struct device_attribute images =
	__ATTR(images, S_IRUGO, qcom_get_images, NULL);


static void socinfo_populate_sysfs_files(struct device *qcom_soc_device)
{
	device_create_file(qcom_soc_device, &qcom_soc_attr_vendor);
	device_create_file(qcom_soc_device, &image_version);
	device_create_file(qcom_soc_device, &image_variant);
	device_create_file(qcom_soc_device, &image_crm_version);
	device_create_file(qcom_soc_device, &select_image);
	device_create_file(qcom_soc_device, &images);

	switch (socinfo_format) {
	case SOCINFO_VERSION(0, 12):
		device_create_file(qcom_soc_device,
					&qcom_soc_attr_chip_family);
		device_create_file(qcom_soc_device,
					&qcom_soc_attr_raw_device_family);
		device_create_file(qcom_soc_device,
					&qcom_soc_attr_raw_device_number);
	case SOCINFO_VERSION(0, 11):
	case SOCINFO_VERSION(0, 10):
		 device_create_file(qcom_soc_device,
					&qcom_soc_attr_serial_number);
	case SOCINFO_VERSION(0, 9):
		 device_create_file(qcom_soc_device,
					&qcom_soc_attr_foundry_id);
	case SOCINFO_VERSION(0, 8):
	case SOCINFO_VERSION(0, 7):
		device_create_file(qcom_soc_device,
					&qcom_soc_attr_pmic_model);
		device_create_file(qcom_soc_device,
					&qcom_soc_attr_pmic_die_revision);
	case SOCINFO_VERSION(0, 6):
		device_create_file(qcom_soc_device,
					&qcom_soc_attr_platform_subtype);
		device_create_file(qcom_soc_device,
					&qcom_soc_attr_platform_subtype_id);
	case SOCINFO_VERSION(0, 5):
		device_create_file(qcom_soc_device,
					&qcom_soc_attr_accessory_chip);
	case SOCINFO_VERSION(0, 4):
		device_create_file(qcom_soc_device,
					&qcom_soc_attr_platform_version);
	case SOCINFO_VERSION(0, 3):
		device_create_file(qcom_soc_device,
					&qcom_soc_attr_hw_platform);
	case SOCINFO_VERSION(0, 2):
		device_create_file(qcom_soc_device,
					&qcom_soc_attr_raw_version);
	case SOCINFO_VERSION(0, 1):
		device_create_file(qcom_soc_device,
					&qcom_soc_attr_build_id);
		break;
	default:
		pr_err("Unknown socinfo format: v%u.%u\n",
				SOCINFO_VERSION_MAJOR(socinfo_format),
				SOCINFO_VERSION_MINOR(socinfo_format));
		break;
	}
}

static void socinfo_populate(struct soc_device_attribute *soc_dev_attr)
{
	u32 soc_version = socinfo_get_version();

	soc_dev_attr->soc_id   = kasprintf(GFP_KERNEL, "%d", socinfo_get_id());
	soc_dev_attr->family  =  "Snapdragon";
	soc_dev_attr->machine  = socinfo_get_id_string();
	soc_dev_attr->revision = kasprintf(GFP_KERNEL, "%u.%u",
			SOCINFO_VERSION_MAJOR(soc_version),
			SOCINFO_VERSION_MINOR(soc_version));
	return;

}

static int socinfo_init_sysfs(void)
{
	struct device *qcom_soc_device;
	struct soc_device *soc_dev;
	struct soc_device_attribute *soc_dev_attr;

	soc_dev_attr = kzalloc(sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr) {
		pr_err("Soc Device alloc failed!\n");
		return -ENOMEM;
	}

	socinfo_populate(soc_dev_attr);
	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR(soc_dev)) {
		kfree(soc_dev_attr);
		pr_err("Soc device register failed\n");
		return PTR_ERR(soc_dev);
	}

	qcom_soc_device = soc_device_to_device(soc_dev);
	socinfo_populate_sysfs_files(qcom_soc_device);
	return 0;
}
#else
static int socinfo_init_sysfs(void)
{
	return 0;
}
#endif

static u32 socinfo_get_id(void)
{
	return (socinfo) ? socinfo->v0_1.id : 0;
}

void *setup_dummy_socinfo(void)
{
	dummy_socinfo.id = MSM_UNKNOWN_ID;
	strlcpy(dummy_socinfo.build_id, "Unknown build",
		sizeof(dummy_socinfo.build_id));
	return (void *) &dummy_socinfo;
}

static void socinfo_print(void)
{
	u32 f_maj = SOCINFO_VERSION_MAJOR(socinfo_format);
	u32 f_min = SOCINFO_VERSION_MINOR(socinfo_format);
	u32 v_maj = SOCINFO_VERSION_MAJOR(socinfo->v0_1.version);
	u32 v_min = SOCINFO_VERSION_MINOR(socinfo->v0_1.version);

	switch (socinfo_format) {
	case SOCINFO_VERSION(0, 1):
		pr_info("v%u.%u, id=%u, ver=%u.%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min);
		break;
	case SOCINFO_VERSION(0, 2):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_ver=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_version);
		break;
	case SOCINFO_VERSION(0, 3):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u\n",
			f_maj, f_min, socinfo->v0_1.id,
			v_maj, v_min, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform);
		break;
	case SOCINFO_VERSION(0, 4):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version);
		break;
	case SOCINFO_VERSION(0, 5):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u accessory_chip=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip);
		break;
	case SOCINFO_VERSION(0, 6):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u accessory_chip=%u, hw_plat_subtype=%u\n",
			f_maj, f_min, socinfo->v0_1.id,
			v_maj, v_min, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype);
		break;
	case SOCINFO_VERSION(0, 7):
	case SOCINFO_VERSION(0, 8):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision);
		break;
	case SOCINFO_VERSION(0, 9):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u foundry_id=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id);
		break;
	case SOCINFO_VERSION(0, 10):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u, foundry_id=%u, serial_number=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
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
	case SOCINFO_VERSION(0, 11):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u, accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u, foundry_id=%u, serial_number=%u num_pmics=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
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
	case SOCINFO_VERSION(0, 12):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u, foundry_id=%u, serial_number=%u, num_pmics=%u, chip_family=0x%x, raw_device_family=0x%x, raw_device_number=0x%x\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
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
		pr_err("Unknown format found: v%u.%u\n", f_maj, f_min);
		break;
	}
}

static void socinfo_select_format(void)
{
	u32 f_maj = SOCINFO_VERSION_MAJOR(socinfo->v0_1.format);
	u32 f_min = SOCINFO_VERSION_MINOR(socinfo->v0_1.format);

	if (f_maj != 0) {
		pr_err("Unsupported format v%u.%u. Falling back to dummy values.\n",
			f_maj, f_min);
		socinfo = setup_dummy_socinfo();
	}

	if (socinfo->v0_1.format > MAX_SOCINFO_FORMAT) {
		pr_warn("Unsupported format v%u.%u. Falling back to v%u.%u.\n",
			f_maj, f_min, SOCINFO_VERSION_MAJOR(MAX_SOCINFO_FORMAT),
			SOCINFO_VERSION_MINOR(MAX_SOCINFO_FORMAT));
		socinfo_format = MAX_SOCINFO_FORMAT;
	} else {
		socinfo_format = socinfo->v0_1.format;
	}
}

int qcom_socinfo_init(void *info, size_t size)
{
	socinfo = info;

	socinfo_select_format();

	WARN(!socinfo_get_id(), "Unknown SOC ID!\n");

	WARN(socinfo_get_id() >= ARRAY_SIZE(cpu_of_id),
		"New IDs added! ID => CPU mapping needs an update.\n");

	socinfo_print();

	socinfo_init_sysfs();

	/* Feed the soc specific unique data into entropy pool */
	add_device_randomness(info, size);

	return 0;
}
EXPORT_SYMBOL(qcom_socinfo_init);

