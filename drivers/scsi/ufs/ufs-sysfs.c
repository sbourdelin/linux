#include <linux/err.h>
#include <linux/string.h>

#include "ufs.h"
#include "ufs-sysfs.h"
/* collision between the device descriptor parameter and the definition */
#undef DEVICE_CLASS

enum ufs_desc_param_size {
	UFS_PARAM_BYTE_SIZE	= 1,
	UFS_PARAM_WORD_SIZE	= 2,
	UFS_PARAM_DWORD_SIZE	= 4,
	UFS_PARAM_QWORD_SIZE	= 8,
};

static inline ssize_t ufs_sysfs_read_desc_param(
	struct ufs_hba *hba, u8 desc_idn, u8 index, char *buf, u8 off,
	enum ufs_desc_param_size param_size)
{
	int desc_len;
	int ret;
	u8 *desc_buf;

	if (ufshcd_map_desc_id_to_length(hba, desc_idn, &desc_len) ||
		off >= desc_len)
		return -EINVAL;
	desc_buf = kzalloc(desc_len, GFP_ATOMIC);
	if (!desc_buf)
		return -ENOMEM;
	ret = ufshcd_query_descriptor_retry(hba, UPIU_QUERY_OPCODE_READ_DESC,
		desc_idn, index, 0, desc_buf, &desc_len);
	if (ret)
		return -EINVAL;
	switch (param_size) {
	case UFS_PARAM_BYTE_SIZE:
		ret = sprintf(buf, "0x%02X\n", desc_buf[off]);
		break;
	case UFS_PARAM_WORD_SIZE:
		ret = sprintf(buf, "0x%04X\n",
			be16_to_cpu(*((u16 *)(desc_buf + off))));
		break;
	case UFS_PARAM_DWORD_SIZE:
		ret = sprintf(buf, "0x%08X\n",
			be32_to_cpu(*((u32 *)(desc_buf + off))));
		break;
	case UFS_PARAM_QWORD_SIZE:
		ret = sprintf(buf, "0x%016llX\n",
			be64_to_cpu(*((u64 *)(desc_buf + off))));
		break;
	}
	kfree(desc_buf);

	return ret;
}

#define ufs_sysfs_desc_param_show(_name, _puname, _duname, _size)             \
static ssize_t _name##_show(struct device *dev,                               \
	struct device_attribute *attr, char *buf)                             \
{                                                                             \
	struct ufs_hba *hba = dev_get_drvdata(dev);                           \
	return ufs_sysfs_read_desc_param(hba, QUERY_DESC_IDN_##_duname,       \
		0, buf, _duname##_DESC_PARAM_##_puname,                       \
		UFS_PARAM_##_size##_SIZE);                                    \
}

#define UFS_DESC_PARAM(_pname, _puname, _duname, _size)                       \
	ufs_sysfs_desc_param_show(_pname, _puname, _duname, _size)            \
	static DEVICE_ATTR_RO(_pname)

#define UFS_DEVICE_DESC_PARAM(_name, _uname, _size)                           \
	UFS_DESC_PARAM(_name, _uname, DEVICE, _size)

UFS_DEVICE_DESC_PARAM(device_type, DEVICE_TYPE, BYTE);
UFS_DEVICE_DESC_PARAM(device_class, DEVICE_CLASS, BYTE);
UFS_DEVICE_DESC_PARAM(device_sub_class, DEVICE_SUB_CLASS, BYTE);
UFS_DEVICE_DESC_PARAM(protocol, PRTCL, BYTE);
UFS_DEVICE_DESC_PARAM(number_of_luns, NUM_LU, BYTE);
UFS_DEVICE_DESC_PARAM(number_of_wluns, NUM_WLU, BYTE);
UFS_DEVICE_DESC_PARAM(boot_enable, BOOT_ENBL, BYTE);
UFS_DEVICE_DESC_PARAM(descriptor_access_enable, DESC_ACCSS_ENBL, BYTE);
UFS_DEVICE_DESC_PARAM(initial_power_mode, INIT_PWR_MODE, BYTE);
UFS_DEVICE_DESC_PARAM(high_priority_lun, HIGH_PR_LUN, BYTE);
UFS_DEVICE_DESC_PARAM(secure_removal_type, SEC_RMV_TYPE, BYTE);
UFS_DEVICE_DESC_PARAM(support_security_lun, SEC_LU, BYTE);
UFS_DEVICE_DESC_PARAM(bkops_termination_latency, BKOP_TERM_LT, BYTE);
UFS_DEVICE_DESC_PARAM(initial_active_icc_level, ACTVE_ICC_LVL, BYTE);
UFS_DEVICE_DESC_PARAM(specification_version, SPEC_VER, WORD);
UFS_DEVICE_DESC_PARAM(manufacturing_date, MANF_DATE, WORD);
UFS_DEVICE_DESC_PARAM(manufacturer_id, MANF_ID, WORD);
UFS_DEVICE_DESC_PARAM(rtt_capability, RTT_CAP, BYTE);
UFS_DEVICE_DESC_PARAM(rtc_update, FRQ_RTC, WORD);
UFS_DEVICE_DESC_PARAM(ufs_features, UFS_FEAT, BYTE);
UFS_DEVICE_DESC_PARAM(ffu_timeout, FFU_TMT, BYTE);
UFS_DEVICE_DESC_PARAM(queue_depth, Q_DPTH, BYTE);
UFS_DEVICE_DESC_PARAM(device_version, DEV_VER, WORD);
UFS_DEVICE_DESC_PARAM(number_of_secure_wpa, NUM_SEC_WPA, BYTE);
UFS_DEVICE_DESC_PARAM(psa_max_data_size, PSA_MAX_DATA, DWORD);
UFS_DEVICE_DESC_PARAM(psa_state_timeout, PSA_TMT, BYTE);

static struct attribute *ufs_sysfs_device_descriptor[] = {
	&dev_attr_device_type.attr,
	&dev_attr_device_class.attr,
	&dev_attr_device_sub_class.attr,
	&dev_attr_protocol.attr,
	&dev_attr_number_of_luns.attr,
	&dev_attr_number_of_wluns.attr,
	&dev_attr_boot_enable.attr,
	&dev_attr_descriptor_access_enable.attr,
	&dev_attr_initial_power_mode.attr,
	&dev_attr_high_priority_lun.attr,
	&dev_attr_secure_removal_type.attr,
	&dev_attr_support_security_lun.attr,
	&dev_attr_bkops_termination_latency.attr,
	&dev_attr_initial_active_icc_level.attr,
	&dev_attr_specification_version.attr,
	&dev_attr_manufacturing_date.attr,
	&dev_attr_manufacturer_id.attr,
	&dev_attr_rtt_capability.attr,
	&dev_attr_rtc_update.attr,
	&dev_attr_ufs_features.attr,
	&dev_attr_ffu_timeout.attr,
	&dev_attr_queue_depth.attr,
	&dev_attr_device_version.attr,
	&dev_attr_number_of_secure_wpa.attr,
	&dev_attr_psa_max_data_size.attr,
	&dev_attr_psa_state_timeout.attr,
	NULL,
};

static const struct attribute_group ufs_sysfs_device_descriptor_group = {
	.name = "device_descriptor",
	.attrs = ufs_sysfs_device_descriptor,
};

#define UFS_INTERCONNECT_DESC_PARAM(_name, _uname, _size)                     \
	UFS_DESC_PARAM(_name, _uname, INTERCONNECT, _size)

UFS_INTERCONNECT_DESC_PARAM(unipro_version, UNIPRO_VER, WORD);
UFS_INTERCONNECT_DESC_PARAM(mphy_version, MPHY_VER, WORD);

static struct attribute *ufs_sysfs_interconnect_descriptor[] = {
	&dev_attr_unipro_version.attr,
	&dev_attr_mphy_version.attr,
	NULL,
};

static const struct attribute_group ufs_sysfs_interconnect_descriptor_group = {
	.name = "interconnect_descriptor",
	.attrs = ufs_sysfs_interconnect_descriptor,
};

#define UFS_GEOMETRY_DESC_PARAM(_name, _uname, _size)                         \
	UFS_DESC_PARAM(_name, _uname, GEOMETRY, _size)

UFS_GEOMETRY_DESC_PARAM(raw_device_capacity, DEV_CAP, QWORD);
UFS_GEOMETRY_DESC_PARAM(max_number_of_luns, MAX_NUM_LUN, BYTE);
UFS_GEOMETRY_DESC_PARAM(segment_size, SEG_SIZE, DWORD);
UFS_GEOMETRY_DESC_PARAM(allocation_unit_size, ALLOC_UNIT_SIZE, BYTE);
UFS_GEOMETRY_DESC_PARAM(min_addressable_block_size, MIN_BLK_SIZE, BYTE);
UFS_GEOMETRY_DESC_PARAM(optimal_read_block_size, OPT_RD_BLK_SIZE, BYTE);
UFS_GEOMETRY_DESC_PARAM(optimal_write_block_size, OPT_RD_BLK_SIZE, BYTE);
UFS_GEOMETRY_DESC_PARAM(max_in_buffer_size, MAX_IN_BUF_SIZE, BYTE);
UFS_GEOMETRY_DESC_PARAM(max_out_buffer_size, MAX_OUT_BUF_SIZE, BYTE);
UFS_GEOMETRY_DESC_PARAM(rpmb_rw_size, RPMB_RW_SIZE, BYTE);
UFS_GEOMETRY_DESC_PARAM(dyn_capacity_resource_policy, DYN_CAP_RSRC_PLC, BYTE);
UFS_GEOMETRY_DESC_PARAM(data_ordering, DATA_ORDER, BYTE);
UFS_GEOMETRY_DESC_PARAM(max_number_of_contexts, MAX_NUM_CTX, BYTE);
UFS_GEOMETRY_DESC_PARAM(sys_data_tag_unit_size, TAG_UNIT_SIZE, BYTE);
UFS_GEOMETRY_DESC_PARAM(sys_data_tag_resource_size, TAG_RSRC_SIZE, BYTE);
UFS_GEOMETRY_DESC_PARAM(secure_removal_types, SEC_RM_TYPES, BYTE);
UFS_GEOMETRY_DESC_PARAM(memory_types, MEM_TYPES, WORD);
UFS_GEOMETRY_DESC_PARAM(sys_code_memory_max_alloc_units,
	SCM_MAX_NUM_UNITS, DWORD);
UFS_GEOMETRY_DESC_PARAM(sys_code_memory_capacity_adjustment_factor,
	SCM_CAP_ADJ_FCTR, WORD);
UFS_GEOMETRY_DESC_PARAM(non_persist_memory_max_alloc_units,
	NPM_MAX_NUM_UNITS, DWORD);
UFS_GEOMETRY_DESC_PARAM(non_persist_memory_capacity_adjustment_factor,
	NPM_CAP_ADJ_FCTR, WORD);
UFS_GEOMETRY_DESC_PARAM(enh1_memory_max_alloc_units,
	ENM1_MAX_NUM_UNITS, DWORD);
UFS_GEOMETRY_DESC_PARAM(enh1_memory_capacity_adjustment_factor,
	ENM1_CAP_ADJ_FCTR, WORD);
UFS_GEOMETRY_DESC_PARAM(enh2_memory_max_alloc_units,
	ENM2_MAX_NUM_UNITS, DWORD);
UFS_GEOMETRY_DESC_PARAM(enh2_memory_capacity_adjustment_factor,
	ENM2_CAP_ADJ_FCTR, WORD);
UFS_GEOMETRY_DESC_PARAM(enh3_memory_max_alloc_units,
	ENM3_MAX_NUM_UNITS, DWORD);
UFS_GEOMETRY_DESC_PARAM(enh3_memory_capacity_adjustment_factor,
	ENM3_CAP_ADJ_FCTR, WORD);
UFS_GEOMETRY_DESC_PARAM(enh4_memory_max_alloc_units,
	ENM4_MAX_NUM_UNITS, DWORD);
UFS_GEOMETRY_DESC_PARAM(enh4_memory_capacity_adjustment_factor,
	ENM4_CAP_ADJ_FCTR, WORD);

static struct attribute *ufs_sysfs_geometry_descriptor[] = {
	&dev_attr_raw_device_capacity.attr,
	&dev_attr_max_number_of_luns.attr,
	&dev_attr_segment_size.attr,
	&dev_attr_allocation_unit_size.attr,
	&dev_attr_min_addressable_block_size.attr,
	&dev_attr_optimal_read_block_size.attr,
	&dev_attr_optimal_write_block_size.attr,
	&dev_attr_max_in_buffer_size.attr,
	&dev_attr_max_out_buffer_size.attr,
	&dev_attr_rpmb_rw_size.attr,
	&dev_attr_dyn_capacity_resource_policy.attr,
	&dev_attr_data_ordering.attr,
	&dev_attr_max_number_of_contexts.attr,
	&dev_attr_sys_data_tag_unit_size.attr,
	&dev_attr_sys_data_tag_resource_size.attr,
	&dev_attr_secure_removal_types.attr,
	&dev_attr_memory_types.attr,
	&dev_attr_sys_code_memory_max_alloc_units.attr,
	&dev_attr_sys_code_memory_capacity_adjustment_factor.attr,
	&dev_attr_non_persist_memory_max_alloc_units.attr,
	&dev_attr_non_persist_memory_capacity_adjustment_factor.attr,
	&dev_attr_enh1_memory_max_alloc_units.attr,
	&dev_attr_enh1_memory_capacity_adjustment_factor.attr,
	&dev_attr_enh2_memory_max_alloc_units.attr,
	&dev_attr_enh2_memory_capacity_adjustment_factor.attr,
	&dev_attr_enh3_memory_max_alloc_units.attr,
	&dev_attr_enh3_memory_capacity_adjustment_factor.attr,
	&dev_attr_enh4_memory_max_alloc_units.attr,
	&dev_attr_enh4_memory_capacity_adjustment_factor.attr,
	NULL,
};

static const struct attribute_group ufs_sysfs_geometry_descriptor_group = {
	.name = "geometry_descriptor",
	.attrs = ufs_sysfs_geometry_descriptor,
};

#define UFS_HEALTH_DESC_PARAM(_name, _uname, _size)                     \
	UFS_DESC_PARAM(_name, _uname, HEALTH, _size)

UFS_HEALTH_DESC_PARAM(eol_info, EOL_INFO, BYTE);
UFS_HEALTH_DESC_PARAM(life_time_estimation_a, LIFE_TIME_EST_A, BYTE);
UFS_HEALTH_DESC_PARAM(life_time_estimation_b, LIFE_TIME_EST_B, BYTE);

static struct attribute *ufs_sysfs_health_descriptor[] = {
	&dev_attr_eol_info.attr,
	&dev_attr_life_time_estimation_a.attr,
	&dev_attr_life_time_estimation_b.attr,
	NULL,
};

static const struct attribute_group ufs_sysfs_health_descriptor_group = {
	.name = "health_descriptor",
	.attrs = ufs_sysfs_health_descriptor,
};

#define ufs_sysfs_power_desc_param_show(_name, _puname, _index)               \
static ssize_t _name##_index##_show(struct device *dev,                       \
	struct device_attribute *attr, char *buf)                             \
{                                                                             \
	struct ufs_hba *hba = dev_get_drvdata(dev);                           \
	return ufs_sysfs_read_desc_param(hba, QUERY_DESC_IDN_POWER, 0, buf,   \
		PWR_DESC_##_puname##_0 + _index * UFS_PARAM_WORD_SIZE,        \
		UFS_PARAM_WORD_SIZE);                                         \
}

#define UFS_POWER_DESC_PARAM(_pname, _puname, _index)                         \
	ufs_sysfs_power_desc_param_show(_pname, _puname, _index)              \
	static DEVICE_ATTR_RO(_pname##_index)

UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 0);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 1);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 2);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 3);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 4);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 5);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 6);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 7);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 8);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 9);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 10);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 11);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 12);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 13);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 14);
UFS_POWER_DESC_PARAM(active_icc_levels_vcc, ACTIVE_LVLS_VCC, 15);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 0);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 1);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 2);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 3);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 4);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 5);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 6);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 7);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 8);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 9);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 10);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 11);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 12);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 13);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 14);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq, ACTIVE_LVLS_VCCQ, 15);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 0);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 1);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 2);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 3);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 4);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 5);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 6);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 7);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 8);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 9);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 10);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 11);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 12);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 13);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 14);
UFS_POWER_DESC_PARAM(active_icc_levels_vccq2, ACTIVE_LVLS_VCCQ2, 15);

static struct attribute *ufs_sysfs_power_descriptor[] = {
	&dev_attr_active_icc_levels_vcc0.attr,
	&dev_attr_active_icc_levels_vcc1.attr,
	&dev_attr_active_icc_levels_vcc2.attr,
	&dev_attr_active_icc_levels_vcc3.attr,
	&dev_attr_active_icc_levels_vcc4.attr,
	&dev_attr_active_icc_levels_vcc5.attr,
	&dev_attr_active_icc_levels_vcc6.attr,
	&dev_attr_active_icc_levels_vcc7.attr,
	&dev_attr_active_icc_levels_vcc8.attr,
	&dev_attr_active_icc_levels_vcc9.attr,
	&dev_attr_active_icc_levels_vcc10.attr,
	&dev_attr_active_icc_levels_vcc11.attr,
	&dev_attr_active_icc_levels_vcc12.attr,
	&dev_attr_active_icc_levels_vcc13.attr,
	&dev_attr_active_icc_levels_vcc14.attr,
	&dev_attr_active_icc_levels_vcc15.attr,
	&dev_attr_active_icc_levels_vccq0.attr,
	&dev_attr_active_icc_levels_vccq1.attr,
	&dev_attr_active_icc_levels_vccq2.attr,
	&dev_attr_active_icc_levels_vccq3.attr,
	&dev_attr_active_icc_levels_vccq4.attr,
	&dev_attr_active_icc_levels_vccq5.attr,
	&dev_attr_active_icc_levels_vccq6.attr,
	&dev_attr_active_icc_levels_vccq7.attr,
	&dev_attr_active_icc_levels_vccq8.attr,
	&dev_attr_active_icc_levels_vccq9.attr,
	&dev_attr_active_icc_levels_vccq10.attr,
	&dev_attr_active_icc_levels_vccq11.attr,
	&dev_attr_active_icc_levels_vccq12.attr,
	&dev_attr_active_icc_levels_vccq13.attr,
	&dev_attr_active_icc_levels_vccq14.attr,
	&dev_attr_active_icc_levels_vccq15.attr,
	&dev_attr_active_icc_levels_vccq20.attr,
	&dev_attr_active_icc_levels_vccq21.attr,
	&dev_attr_active_icc_levels_vccq22.attr,
	&dev_attr_active_icc_levels_vccq23.attr,
	&dev_attr_active_icc_levels_vccq24.attr,
	&dev_attr_active_icc_levels_vccq25.attr,
	&dev_attr_active_icc_levels_vccq26.attr,
	&dev_attr_active_icc_levels_vccq27.attr,
	&dev_attr_active_icc_levels_vccq28.attr,
	&dev_attr_active_icc_levels_vccq29.attr,
	&dev_attr_active_icc_levels_vccq210.attr,
	&dev_attr_active_icc_levels_vccq211.attr,
	&dev_attr_active_icc_levels_vccq212.attr,
	&dev_attr_active_icc_levels_vccq213.attr,
	&dev_attr_active_icc_levels_vccq214.attr,
	&dev_attr_active_icc_levels_vccq215.attr,
	NULL,
};

static const struct attribute_group ufs_sysfs_power_descriptor_group = {
	.name = "power_descriptor",
	.attrs = ufs_sysfs_power_descriptor,
};

#define ufs_sysfs_string_descriptor_show(_name, _pname)                       \
static ssize_t _name##_show(struct device *dev,                               \
	struct device_attribute *attr, char *buf)                             \
{                                                                             \
	u8 index;                                                             \
	struct ufs_hba *hba = dev_get_drvdata(dev);                           \
	int ret;                                                              \
	int desc_len = QUERY_DESC_MAX_SIZE;                                   \
	u8 *desc_buf;                                                         \
	desc_buf = kzalloc(QUERY_DESC_MAX_SIZE, GFP_ATOMIC);                  \
	if (!desc_buf)                                                        \
		return -ENOMEM;                                               \
	ret = ufshcd_query_descriptor_retry(hba, UPIU_QUERY_OPCODE_READ_DESC, \
		QUERY_DESC_IDN_DEVICE, 0, 0, desc_buf, &desc_len);            \
	if (ret) {                                                            \
		ret = -EINVAL;                                                \
		goto out;                                                     \
	}                                                                     \
	index = desc_buf[DEVICE_DESC_PARAM_##_pname];                         \
	memset(desc_buf, 0, QUERY_DESC_MAX_SIZE);                             \
	if (ufshcd_read_string_desc(hba, index, desc_buf,                     \
		QUERY_DESC_MAX_SIZE, true)) {                                 \
		ret = -EINVAL;                                                \
		goto out;                                                     \
	}                                                                     \
	ret = snprintf(buf, PAGE_SIZE, "%s\n",                                \
		desc_buf + QUERY_DESC_HDR_SIZE);                              \
out:                                                                          \
	kfree(desc_buf);                                                      \
	return ret;                                                           \
}

#define UFS_STRING_DESCRIPTOR(_name, _pname)                                  \
	ufs_sysfs_string_descriptor_show(_name, _pname)                       \
	static DEVICE_ATTR_RO(_name)

UFS_STRING_DESCRIPTOR(manufacturer_name, MANF_NAME);
UFS_STRING_DESCRIPTOR(product_name, PRDCT_NAME);
UFS_STRING_DESCRIPTOR(oem_id, OEM_ID);
UFS_STRING_DESCRIPTOR(serial_number, SN);
UFS_STRING_DESCRIPTOR(product_revision, PRDCT_REV);

static struct attribute *ufs_sysfs_string_descriptors[] = {
	&dev_attr_manufacturer_name.attr,
	&dev_attr_product_name.attr,
	&dev_attr_oem_id.attr,
	&dev_attr_serial_number.attr,
	&dev_attr_product_revision.attr,
	NULL,
};

static const struct attribute_group ufs_sysfs_string_descriptors_group = {
	.name = "string_descriptors",
	.attrs = ufs_sysfs_string_descriptors,
};


static const struct attribute_group *ufs_sysfs_groups[] = {
	&ufs_sysfs_device_descriptor_group,
	&ufs_sysfs_interconnect_descriptor_group,
	&ufs_sysfs_geometry_descriptor_group,
	&ufs_sysfs_health_descriptor_group,
	&ufs_sysfs_power_descriptor_group,
	&ufs_sysfs_string_descriptors_group,
	NULL,
};

#define ufs_sysfs_lun_desc_param_show(_pname, _puname, _duname, _size)        \
static ssize_t _pname##_show(struct device *dev,                              \
	struct device_attribute *attr, char *buf)                             \
{                                                                             \
	struct scsi_device *sdev = to_scsi_device(dev);                       \
	struct ufs_hba *hba = shost_priv(sdev->host);                         \
	u8 lun = ufshcd_scsi_to_upiu_lun(sdev->lun);                          \
	if (!ufs_is_valid_unit_desc_lun(lun))                                 \
		return -EINVAL;                                               \
	return ufs_sysfs_read_desc_param(hba, QUERY_DESC_IDN_##_duname,       \
		lun, buf, _duname##_DESC_PARAM_##_puname,                     \
		UFS_PARAM_##_size##_SIZE);                                    \
}

#define UFS_LUN_DESC_PARAM(_pname, _puname, _duname, _size)                   \
	ufs_sysfs_lun_desc_param_show(_pname, _puname, _duname, _size)        \
	static DEVICE_ATTR_RO(_pname)

#define UFS_UNIT_DESC_PARAM(_name, _uname, _size)                             \
	UFS_LUN_DESC_PARAM(_name, _uname, UNIT, _size)

UFS_UNIT_DESC_PARAM(boot_lun_id, BOOT_LUN_ID, BYTE);
UFS_UNIT_DESC_PARAM(lun_write_protect, LU_WR_PROTECT, BYTE);
UFS_UNIT_DESC_PARAM(lun_queue_depth, LU_Q_DEPTH, BYTE);
UFS_UNIT_DESC_PARAM(psa_sensitive, PSA_SENSITIVE, BYTE);
UFS_UNIT_DESC_PARAM(lun_memory_type, MEM_TYPE, BYTE);
UFS_UNIT_DESC_PARAM(data_reliability, DATA_RELIABILITY, BYTE);
UFS_UNIT_DESC_PARAM(logical_block_size, LOGICAL_BLK_SIZE, BYTE);
UFS_UNIT_DESC_PARAM(logical_block_count, LOGICAL_BLK_COUNT, QWORD);
UFS_UNIT_DESC_PARAM(erase_block_size, ERASE_BLK_SIZE, DWORD);
UFS_UNIT_DESC_PARAM(provisioning_type, PROVISIONING_TYPE, BYTE);
UFS_UNIT_DESC_PARAM(physical_memory_resourse_count, PHY_MEM_RSRC_CNT, QWORD);
UFS_UNIT_DESC_PARAM(context_capabilities, CTX_CAPABILITIES, WORD);
UFS_UNIT_DESC_PARAM(large_unit_granularity, LARGE_UNIT_SIZE_M1, BYTE);

static struct attribute *ufs_sysfs_unit_descriptor[] = {
	&dev_attr_boot_lun_id.attr,
	&dev_attr_lun_write_protect.attr,
	&dev_attr_lun_queue_depth.attr,
	&dev_attr_psa_sensitive.attr,
	&dev_attr_lun_memory_type.attr,
	&dev_attr_data_reliability.attr,
	&dev_attr_logical_block_size.attr,
	&dev_attr_logical_block_count.attr,
	&dev_attr_erase_block_size.attr,
	&dev_attr_provisioning_type.attr,
	&dev_attr_physical_memory_resourse_count.attr,
	&dev_attr_context_capabilities.attr,
	&dev_attr_large_unit_granularity.attr,
	NULL,
};

struct attribute_group ufs_sysfs_unit_descriptor_group = {
	.name = "unit_descriptor",
	.attrs = ufs_sysfs_unit_descriptor,
};
EXPORT_SYMBOL(ufs_sysfs_unit_descriptor_group);

void ufs_sysfs_add_device_management(struct ufs_hba *hba)
{
	int ret;

	ret = sysfs_create_groups(&hba->dev->kobj, ufs_sysfs_groups);
	if (ret)
		dev_err(hba->dev,
			"%s: sysfs groups creation failed (err = %d)\n",
			__func__, ret);
}
EXPORT_SYMBOL(ufs_sysfs_add_device_management);

void ufs_sysfs_remove_device_management(struct ufs_hba *hba)
{
	sysfs_remove_groups(&hba->dev->kobj, ufs_sysfs_groups);
}
EXPORT_SYMBOL(ufs_sysfs_remove_device_management);

MODULE_LICENSE("GPL");
