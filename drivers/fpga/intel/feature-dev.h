/*
 * Intel FPGA Feature Device Driver Header File
 *
 * Copyright (C) 2017 Intel Corporation, Inc.
 *
 * Authors:
 *   Kang Luwei <luwei.kang@intel.com>
 *   Zhang Yi <yi.z.zhang@intel.com>
 *   Wu Hao <hao.wu@intel.com>
 *   Xiao Guangrong <guangrong.xiao@linux.intel.com>
 *
 * This work is licensed under a dual BSD/GPLv2 license. When using or
 * redistributing this file, you may do so under either license. See the
 * LICENSE.BSD file under this directory for the BSD license and see
 * the COPYING file in the top-level directory for the GPLv2 license.
 */

#ifndef __INTEL_FPGA_FEATURE_H
#define __INTEL_FPGA_FEATURE_H

#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/pci.h>
#include <linux/uuid.h>
#include <linux/delay.h>
#include <linux/platform_device.h>

/* maximum supported number of ports */
#define MAX_FPGA_PORT_NUM 4
/* plus one for fme device */
#define MAX_FEATURE_DEV_NUM	(MAX_FPGA_PORT_NUM + 1)

#define FME_FEATURE_HEADER          "fme_hdr"
#define FME_FEATURE_THERMAL_MGMT    "fme_thermal"
#define FME_FEATURE_POWER_MGMT      "fme_power"
#define FME_FEATURE_GLOBAL_PERF     "fme_gperf"
#define FME_FEATURE_GLOBAL_ERR      "fme_error"
#define FME_FEATURE_PR_MGMT         "fme_pr"

#define PORT_FEATURE_HEADER         "port_hdr"
#define PORT_FEATURE_UAFU           "port_uafu"
#define PORT_FEATURE_ERR            "port_err"
#define PORT_FEATURE_UMSG           "port_umsg"
#define PORT_FEATURE_PR             "port_pr"
#define PORT_FEATURE_STP            "port_stp"

/* All headers and structures must be byte-packed to match the spec. */
#pragma pack(1)

/* common header for all features */
struct feature_header {
	union {
		u64 csr;
		struct {
			u16 id:12;
			u8  revision:4;
			u32 next_header_offset:24; /* offset to next header */
			u32 rsvdz:20;
			u8  type:4;		   /* feature type */
#define FEATURE_TYPE_AFU		0x1
#define FEATURE_TYPE_PRIVATE		0x3
		};
	};
};

/* common header for non-private features */
struct feature_afu_header {
	uuid_le guid;
	union {
		u64 csr;
		struct {
			u64 next_afu:24;	/* pointer to next afu header */
			u64 rsvdz:40;
		};
	};
};

/* FME Header Register Set */
/* FME Capability Register */
struct feature_fme_capability {
	union {
		u64 csr;
		struct {
			u8  fabric_verid;	/* Fabric version ID */
			u8  socket_id:1;	/* Socket id */
			u8  rsvdz1:3;
			u8  pcie0_link_avl:1;	/* PCIe0 link availability */
			u8  pcie1_link_avl:1;	/* PCIe1 link availability */
			u8  coherent_link_avl:1;/* Coherent link availability */
			u8  rsvdz2:1;
			u8  iommu_support:1;	/* IOMMU or VT-d supported */
			u8  num_ports:3;	/* Num of ports implemented */
			u8  rsvdz3:4;
			u8  addr_width_bits:6;	/* Address width supported */
			u8  rsvdz4:2;
			u16 cache_size:12;	/* Cache size in kb */
			u8  cache_assoc:4;	/* Cache Associativity */
			u16 rsvdz5:15;
			u8  lock_bit:1;		/* Latched lock bit by BIOS */
		};
	};
};

/* FME Port Offset Register */
struct feature_fme_port {
	union {
		u64 csr;
		struct {
			u32 port_offset:24;	/* Offset to port header */
			u8  rsvdz1;
			u8  port_bar:3;		/* Bar id */
			u32 rsvdz2:20;
			u8  afu_access_ctrl:1;	/* AFU access type: PF/VF */
			u8  rsvdz3:4;
			u8  port_implemented:1;	/* Port implemented or not */
			u8  rsvdz4:3;
		};
	};
};

struct feature_fme_header {
	struct feature_header header;
	struct feature_afu_header afu_header;
	u64 rsvd[2];
	struct feature_fme_capability capability;
	struct feature_fme_port port[MAX_FPGA_PORT_NUM];
};

/* FME Thermal Sub Feature Register Set */
struct feature_fme_thermal {
	struct feature_header header;
};

/* FME Power Sub Feature Register Set */
struct feature_fme_power {
	struct feature_header header;
};

/* FME Global Performance Sub Feature Register Set */
struct feature_fme_gperf {
	struct feature_header header;
};

/* FME Error Sub Feature Register Set */
struct feature_fme_err {
	struct feature_header header;
};

/* FME Partial Reconfiguration Sub Feature Register Set */
struct feature_fme_pr {
	struct feature_header header;
};

/* PORT Header Register Set */
/* Port Capability Register */
struct feature_port_capability {
	union {
		u64 csr;
		struct {
			u8  port_number:2;	/* Port Number 0-3 */
			u8  rsvdz1:6;
			u16 mmio_size;		/* User MMIO size in KB */
			u8  rsvdz2;
			u8  sp_intr_num:4;	/* Supported interrupts num */
			u32 rsvdz3:28;
		};
	};
};

/* Port Control Register */
struct feature_port_control {
	union {
		u64 csr;
		struct {
			u8  port_sftrst:1;	/* Port Soft Reset */
			u8  rsvdz1:1;
			u8  latency_tolerance:1;/* '1' >= 40us, '0' < 40us */
			u8  rsvdz2:1;
			u8  port_sftrst_ack:1;	/* HW ACK for Soft Reset */
			u64 rsvdz3:59;
		};
	};
};

struct feature_port_header {
	struct feature_header header;
	struct feature_afu_header afu_header;
	u64 rsvd[2];
	struct feature_port_capability capability;
	struct feature_port_control control;
};

/* PORT Error Sub Feature Register Set */
struct feature_port_error {
	struct feature_header header;
};

/* PORT Unordered Message Sub Feature Register Set */
struct feature_port_umsg {
	struct feature_header header;
};

/* PORT SignalTap Sub Feature Register Set */
struct feature_port_stp {
	struct feature_header header;
};

#pragma pack()

struct feature {
	const char *name;
	int resource_index;
	void __iomem *ioaddr;
};

struct feature_platform_data {
	/* list the feature dev to cci_drvdata->port_dev_list. */
	struct list_head node;
	struct mutex lock;
	struct cdev cdev;
	struct platform_device *dev;
	unsigned int disable_count;	/* count for port disable */

	int num;			/* number of features */
	struct feature features[0];
};

enum fme_feature_id {
	FME_FEATURE_ID_HEADER = 0x0,
	FME_FEATURE_ID_THERMAL_MGMT = 0x1,
	FME_FEATURE_ID_POWER_MGMT = 0x2,
	FME_FEATURE_ID_GLOBAL_PERF = 0x3,
	FME_FEATURE_ID_GLOBAL_ERR = 0x4,
	FME_FEATURE_ID_PR_MGMT = 0x5,
	FME_FEATURE_ID_MAX = 0x6,
};

enum port_feature_id {
	PORT_FEATURE_ID_HEADER = 0x0,
	PORT_FEATURE_ID_ERROR = 0x1,
	PORT_FEATURE_ID_UMSG = 0x2,
	PORT_FEATURE_ID_PR = 0x3,
	PORT_FEATURE_ID_STP = 0x4,
	PORT_FEATURE_ID_UAFU = 0x5,
	PORT_FEATURE_ID_MAX = 0x6,
};

int fme_feature_num(void);
int port_feature_num(void);

#define FPGA_FEATURE_DEV_FME		"intel-fpga-fme"
#define FPGA_FEATURE_DEV_PORT		"intel-fpga-port"

void feature_platform_data_add(struct feature_platform_data *pdata,
			       int index, const char *name,
			       int resource_index, void __iomem *ioaddr);
int feature_platform_data_size(int num);
struct feature_platform_data *
feature_platform_data_alloc_and_init(struct platform_device *dev, int num);

enum fpga_devt_type {
	FPGA_DEVT_FME,
	FPGA_DEVT_PORT,
	FPGA_DEVT_MAX,
};

void fpga_chardev_uinit(void);
int fpga_chardev_init(void);
dev_t fpga_get_devt(enum fpga_devt_type type, int id);
int fpga_register_dev_ops(struct platform_device *pdev,
			  const struct file_operations *fops,
			  struct module *owner);
void fpga_unregister_dev_ops(struct platform_device *pdev);

int fpga_port_id(struct platform_device *pdev);

static inline int fpga_port_check_id(struct platform_device *pdev,
				     void *pport_id)
{
	return fpga_port_id(pdev) == *(int *)pport_id;
}

void __fpga_port_enable(struct platform_device *pdev);
int __fpga_port_disable(struct platform_device *pdev);

static inline void fpga_port_enable(struct platform_device *pdev)
{
	struct feature_platform_data *pdata = dev_get_platdata(&pdev->dev);

	mutex_lock(&pdata->lock);
	__fpga_port_enable(pdev);
	mutex_unlock(&pdata->lock);
}

static inline int fpga_port_disable(struct platform_device *pdev)
{
	struct feature_platform_data *pdata = dev_get_platdata(&pdev->dev);
	int ret;

	mutex_lock(&pdata->lock);
	ret = __fpga_port_disable(pdev);
	mutex_unlock(&pdata->lock);

	return ret;
}

static inline int __fpga_port_reset(struct platform_device *pdev)
{
	int ret;

	ret = __fpga_port_disable(pdev);
	if (ret)
		return ret;

	__fpga_port_enable(pdev);
	return 0;
}

static inline int fpga_port_reset(struct platform_device *pdev)
{
	struct feature_platform_data *pdata = dev_get_platdata(&pdev->dev);
	int ret;

	mutex_lock(&pdata->lock);
	ret = __fpga_port_reset(pdev);
	mutex_unlock(&pdata->lock);
	return ret;
}

static inline void __iomem *
get_feature_ioaddr_by_index(struct device *dev, int index)
{
	struct feature_platform_data *pdata = dev_get_platdata(dev);

	return pdata->features[index].ioaddr;
}

/*
 * Wait register's _field to be changed to the given value (_expect's _field)
 * by polling with given interval and timeout.
 */
#define fpga_wait_register_field(_field, _expect, _reg_addr, _timeout, _invl)\
({									     \
	int wait = 0;							     \
	int ret = -ETIMEDOUT;						     \
	typeof(_expect) value;						     \
	for (; wait <= _timeout; wait += _invl) {			     \
		value.csr = readq(_reg_addr);				     \
		if (_expect._field == value._field) {			     \
			ret = 0;					     \
			break;						     \
		}							     \
		udelay(_invl);						     \
	}								     \
	ret;								     \
})

#endif
