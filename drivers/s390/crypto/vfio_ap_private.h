/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Private data and functions for adjunct processor VFIO matrix driver.
 *
 * Author(s): Tony Krowiak <akrowiak@linux.ibm.com>
 *	      Halil Pasic <pasic@linux.ibm.com>
 *
 * Copyright IBM Corp. 2018
 */

#ifndef _VFIO_AP_PRIVATE_H_
#define _VFIO_AP_PRIVATE_H_

#include <linux/types.h>
#include <linux/device.h>
#include <linux/mdev.h>
#include <linux/delay.h>
#include <linux/mutex.h>

#include "ap_bus.h"

#define VFIO_AP_MODULE_NAME "vfio_ap"
#define VFIO_AP_DRV_NAME "vfio_ap"

/**
 * ap_matrix_dev - the AP matrix device structure
 * @device:	generic device structure associated with the AP matrix device
 * @available_instances: number of mediated matrix devices that can be created
 * @info:	the struct containing the output from the PQAP(QCI) instruction
 * mdev_list:	the list of mediated matrix devices created
 * lock:	mutex for locking the AP matrix device. This lock will be
 *		taken every time we fiddle with state managed by the vfio_ap
 *		driver, be it using @mdev_list or writing the state of a
 *		single ap_matrix_mdev device. It's quite coarse but we don't
 *		expect much contention.
 */
struct ap_matrix_dev {
	struct device device;
	atomic_t available_instances;
	struct ap_config_info info;
	struct list_head mdev_list;
	struct mutex lock;
};

extern struct ap_matrix_dev *matrix_dev;

/**
 * The AP matrix is comprised of three bit masks identifying the adapters,
 * queues (domains) and control domains that belong to an AP matrix. The bits i
 * each mask, from least significant to most significant bit, correspond to IDs
 * 0 to 255. When a bit is set, the corresponding ID belongs to the matrix.
 *
 * @apm_max: max adapter number in @apm
 * @apm identifies the AP adapters in the matrix
 * @aqm_max: max domain number in @aqm
 * @aqm identifies the AP queues (domains) in the matrix
 * @adm_max: max domain number in @adm
 * @adm identifies the AP control domains in the matrix
 */
struct ap_matrix {
	unsigned long apm_max;
	DECLARE_BITMAP(apm, 256);
	unsigned long aqm_max;
	DECLARE_BITMAP(aqm, 256);
	unsigned long adm_max;
	DECLARE_BITMAP(adm, 256);
};

/**
 * struct ap_matrix_mdev - the mediated matrix device structure
 * @list:	allows the ap_matrix_mdev struct to be added to a list
 * @matrix:	the adapters, usage domains and control domains assigned to the
 *		mediated matrix device.
 * @group_notifier: notifier block used for specifying callback function for
 *		    handling the VFIO_GROUP_NOTIFY_SET_KVM event
 * @kvm:	the struct holding guest's state
 * @map:	the adapter information for QEMU mapping
 * @gisc:	the Guest ISC
 */
struct ap_matrix_mdev {
	struct list_head node;
	struct ap_matrix matrix;
	struct notifier_block group_notifier;
	struct kvm *kvm;
	struct s390_map_info *map;
	unsigned char gisc;
};

extern int vfio_ap_mdev_register(void);
extern void vfio_ap_mdev_unregister(void);

/* AP Queue Interrupt Control associated structures and functions */
struct aqic_gisa {
	uint8_t  rzone;
	uint8_t  izone;
	unsigned	ir:1;
	unsigned	reserved1:4;
	unsigned	gisc:3;
	unsigned	reserved2:6;
	unsigned	f:2;
	unsigned	reserved3:1;
	unsigned	gisao:27;
	unsigned	t:1;
	unsigned	isc:3;
}  __packed __aligned(8);

struct ap_status {
	unsigned	e:1;
	unsigned	r:1;
	unsigned	f:1;
	unsigned	reserved:4;
	unsigned	i:1;
	unsigned	rc:8;
	unsigned	pad:16;
}  __packed __aligned(4);

static inline uint32_t status2reg(struct ap_status a)
{
	return *(uint32_t *)(&a);
}

static inline struct ap_status reg2status(uint32_t r)
{
	return *(struct ap_status *)(&r);
}

static inline struct aqic_gisa reg2aqic(uint64_t r)
{
	return *((struct aqic_gisa *)&r);
}

static inline uint64_t aqic2reg(struct aqic_gisa a)
{
	return *((uint64_t *)&a);
}

/**
 * ap_host_aqic - Issue the host AQIC instruction.
 * @apqn is the AP queue number
 * @gr1  the caller must have setup the register
 *       with GISA address and format, with interrupt
 *       request, ISC and guest ISC
 * @gr2  the caller must have setup the register
 *       to the guest NIB physical address
 *
 * issue the AQIC PQAP instruction and return the AP status
 * word
 */
static inline uint32_t ap_host_aqic(uint64_t apqn, uint64_t gr1,
				    uint64_t gr2)
{
	register unsigned long reg0 asm ("0") = apqn | (3UL << 24);
	register unsigned long reg1_in asm ("1") = gr1;
	register uint32_t reg1_out asm ("1");
	register unsigned long reg2 asm ("2") = gr2;

	asm volatile(
		".long 0xb2af0000"	  /* PQAP(AQIC) */
		: "+d" (reg0), "+d" (reg1_in), "=d" (reg1_out), "+d" (reg2)
		:
		: "cc");
	return reg1_out;
}

#endif /* _VFIO_AP_PRIVATE_H_ */
