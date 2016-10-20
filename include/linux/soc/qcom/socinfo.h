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

#ifndef _ARCH_ARM_MACH_MSM_SOCINFO_H_
#define _ARCH_ARM_MACH_MSM_SOCINFO_H_

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/of_fdt.h>
#include <linux/of.h>

#include <asm/cputype.h>
/*
 * SOC version type with major number in the upper 16 bits and minor
 * number in the lower 16 bits.  For example:
 *   1.0 -> 0x00010000
 *   2.3 -> 0x00020003
 */
#define SOCINFO_VERSION_MAJOR(ver) (((ver) & 0xffff0000) >> 16)
#define SOCINFO_VERSION_MINOR(ver) ((ver) & 0x0000ffff)
#define SOCINFO_VERSION(maj, min)  ((((maj) & 0xffff) << 16)|((min) & 0xffff))

#ifdef CONFIG_OF
#define early_machine_is_apq8064()	\
	of_flat_dt_is_compatible(of_get_flat_dt_root(), "qcom,apq8064")
#define early_machine_is_apq8084()	\
	of_flat_dt_is_compatible(of_get_flat_dt_root(), "qcom,apq8084")
#define early_machine_is_msm8916()	\
	of_flat_dt_is_compatible(of_get_flat_dt_root(), "qcom,msm8916")
#define early_machine_is_msm8660()	\
	of_flat_dt_is_compatible(of_get_flat_dt_root(), "qcom,msm8660")
#define early_machine_is_msm8960()	\
	of_flat_dt_is_compatible(of_get_flat_dt_root(), "qcom,msm8960")
#define early_machine_is_msm8974()	\
	of_flat_dt_is_compatible(of_get_flat_dt_root(), "qcom,msm8974")
#define early_machine_is_msm8996()	\
	of_flat_dt_is_compatible(of_get_flat_dt_root(), "qcom,msm8996")
#else
#define early_machine_is_apq8064()	0
#define early_machine_is_apq8084()	0
#define early_machine_is_msm8916()	0
#define early_machine_is_msm8660()	0
#define early_machine_is_msm8960()	0
#define early_machine_is_msm8974()	0
#define early_machine_is_msm8996()	0
#endif

#define PLATFORM_SUBTYPE_MDM	1
#define PLATFORM_SUBTYPE_INTERPOSERV3 2
#define PLATFORM_SUBTYPE_SGLTE	6

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

enum msm_cpu socinfo_get_msm_cpu(void);
uint32_t socinfo_get_id(void);
uint32_t socinfo_get_version(void);
uint32_t socinfo_get_raw_id(void);
char *socinfo_get_build_id(void);
uint32_t socinfo_get_platform_type(void);
uint32_t socinfo_get_platform_subtype(void);
uint32_t socinfo_get_platform_version(void);
uint32_t socinfo_get_serial_number(void);
enum qcom_pmic_model socinfo_get_pmic_model(void);
uint32_t socinfo_get_pmic_die_revision(void);
int __init socinfo_init(void) __must_check;

#endif
