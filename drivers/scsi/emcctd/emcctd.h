/*
 * EMCCTD: EMC Cut-Through HBA Driver for SCSI subsystem.
 *
 * Copyright (C) 2015 by EMC Corporation, Hopkinton, MA.
 *
 * Authors:
 *	fredette, matt <matt.fredette@emc.com>
 *	Pirotte, Serge <serge.pirotte@emc.com>
 *	Singh Animesh <Animesh.Singh@emc.com>
 *	Singhal, Maneesh <Maneesh.Singhal@emc.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _EMCCTD_H_
#define _EMCCTD_H_

#define DRV_NAME	"emcctd"

#define EMCCTD_V010_PROTOCOL_MINOR_VERSION	0x0

/* please refer to emc_ctd_v010_scsi_command_cdb in emc_ctd_interface.h */
#define EMCCTD_V010_MAX_CDB_SIZE		16

#define EMCCTD_MAX_LUN			16384
#define EMCCTD_MAX_ID			16
#define EMCCTD_MAX_RETRY		5
#define EMCCTD_CMD_PER_LUN		16
#define EMCCTD_THIS_ID			(-1)
#define EMCCTD_REQUEST_TIMEOUT		(60 * HZ)
#define EMCCTD_OPAQUE_PURGE_WAITTIME	(10 * HZ)

#define EMCCTD_DEVICE_RESET_PAUSE	3
#define EMCCTD_DETECT_RETRY_MAX		3

#define ctd_dprintk(__m_fmt, ...)		\
do {				\
	if (ctd_debug)		\
		pr_info("%s:%d:"__m_fmt, __func__, __LINE__, ##__VA_ARGS__); \
} while (0)

#define ctd_dprintk_crit(__m_fmt, ...)		\
		pr_crit("%s:%d:"__m_fmt, __func__, __LINE__, ##__VA_ARGS__)

#define EMCCTD_TARGET_DETECT_COMPLETED		1
#define EMCCTD_TARGET_DETECT_NOT_COMPLETED	0

struct ctd_target_info {
	unsigned int detect_completed;
	struct scsi_target *starget;
	struct emc_ctd_v010_detect ctd_detect;
};

#define emc_ctd_detect_header_address           \
		emc_ctd_v010_detect_header.emc_ctd_v010_header_address
#define emc_ctd_detect_header_minor             \
		emc_ctd_v010_detect_header.emc_ctd_v010_header_minor
#define emc_ctd_detect_header_what              \
		emc_ctd_v010_detect_header.emc_ctd_v010_header_what

#define emc_ctd_scsi_command_header_address     \
		emc_ctd_v010_scsi_command_header.emc_ctd_v010_header_address
#define emc_ctd_scsi_command_header_minor       \
		emc_ctd_v010_scsi_command_header.emc_ctd_v010_header_minor
#define emc_ctd_scsi_command_header_what        \
		emc_ctd_v010_scsi_command_header.emc_ctd_v010_header_what

#define emc_ctd_scsi_response_header_address    \
		emc_ctd_v010_scsi_response_header.emc_ctd_v010_header_address
#define emc_ctd_scsi_response_header_minor      \
		emc_ctd_v010_scsi_response_header.emc_ctd_v010_header_minor
#define emc_ctd_scsi_response_header_what       \
		emc_ctd_v010_scsi_response_header.emc_ctd_v010_header_what

#define emc_ctd_scsi_phase_header_address       \
		emc_ctd_v010_scsi_phase_header.emc_ctd_v010_header_address
#define emc_ctd_scsi_phase_header_minor         \
		emc_ctd_v010_scsi_phase_header.emc_ctd_v010_header_minor
#define emc_ctd_scsi_phase_header_what          \
		emc_ctd_v010_scsi_phase_header.emc_ctd_v010_header_what

#define emc_ctd_scsi_message_header_address     \
		emc_ctd_v010_message_header.emc_ctd_v010_header_address
#define emc_ctd_scsi_message_header_minor       \
		emc_ctd_v010_message_header.emc_ctd_v010_header_minor
#define emc_ctd_scsi_message_header_what        \
		emc_ctd_v010_message_header.emc_ctd_v010_header_what

#define emc_ctd_scsi_command_sgl                \
		emc_ctd_v010_scsi_command_u.emc_ctd_v010_scsi_command_sgl
#define emc_ctd_scsi_response_extra             \
		emc_ctd_v010_scsi_response_u.emc_ctd_v010_scsi_response_extra

#define ctd_detect_name_bytes		\
	ctd_detect.emc_ctd_v010_detect_name.emc_ctd_v010_name_bytes

struct ctd_host_info {
	struct Scsi_Host *shost;
	struct pci_dev *pci_dev;
	struct ctd_target_info target[EMCCTD_MAX_ID];
};

struct ctd_dev_info {
	struct ctd_host_info *ctd_host;
	struct ctd_target_info *ctd_target;
	struct emc_ctd_v010_detect *ctd_target_detect;
};

#define PROC_STAT_SCSI_TS_MAX   10
#define MAX_PROC_FILE_NAMELEN	128
#define CTD_MAX_IO_STATS	200

struct ctd_hw_stats {
	atomic_long_t			interrupts;
	atomic_long_t			requests_sent;
	atomic_long_t			responses_received;
	atomic_long_t			active_io_count;
	atomic_long_t			abort_sent;
	atomic_long_t			abort_received;
	atomic_long_t			what_in;
	atomic_long_t			what_out;
	atomic_long_t			free_io_entries;
	unsigned long long		io_stats[CTD_MAX_IO_STATS];
	unsigned int			io_stats_index;
};

enum ctd_io_request_state {
	CTD_IO_REQUEST_FREE,
	CTD_IO_REQUEST_QUEUED,
	CTD_IO_REQUEST_REQUEUED,
	CTD_IO_REQUEST_ABORTED,
	CTD_IO_REQUEST_COMPLETED,
	CTD_IO_REQUEST_REPLY_AWAITED,
	CTD_IO_REQUEST_INVALID
};

enum ctd_hw_state {
	CTD_HW_STATE_UNINITIALIZED,
	CTD_HW_STATE_INITIALIZED,
	CTD_HW_STATE_UNDER_RESET,
	CTD_HW_STATE_DISABLED,
	CTD_HW_STATE_INVALID
};

struct ctd_request_private {
	struct list_head		list;
	enum ctd_io_request_state	io_requeue_state;
	unsigned int			io_timeout;
	enum ctd_io_request_state	io_state;
	struct scsi_cmnd		*cmnd;
	struct page			*cdb_page;
	unsigned int			cdb_page_order;
	struct page			*sgllist_page;
	unsigned int			sgllist_page_order;
	unsigned long			purge_lifetime;
	unsigned long long		io_start_time;
};

struct ctd_pci_private {
	struct pci_dev			*pci_dev;
	void				*host_private;

	void __iomem			*ioaddr_txrx_rings;
	void __iomem			*ioaddr_fast_registers;
	void __iomem			*ioaddr_slow_registers;

	emc_ctd_uint32_t		txrx_ringsize;
	emc_ctd_uint32_t		pci_request_queue_size;
	emc_ctd_uint32_t		pci_response_queue_size;
	struct emc_ctd_v010_fregs	*pci_fast_registers;
	struct emc_ctd_v010_sregs	*pci_slow_registers;
	union emc_ctd_v010_message	*pci_response_array;
	union emc_ctd_v010_message	*pci_request_array;

	struct tasklet_struct		isr_tasklet;
	spinlock_t			isr_lock;
	unsigned int			hw_index;
	struct ctd_hw_stats		hw_stats;
	enum ctd_hw_state		hw_state;
	struct list_head		queued_io_list;
	struct list_head		aborted_io_list;
	struct list_head		requeued_io_list;
	struct list_head		io_pool;
	struct ctd_request_private	*io_map;
	struct ctd_request_private	*io_map_end;
	spinlock_t			io_mgmt_lock;

	struct task_struct		*ctd_event_thread;
	struct list_head		event_io_list;
	spinlock_t			event_io_lock;
};

#define request_producer_index		\
		pci_fast_registers->emc_ctd_v010_fregs_tx_index_producer
#define request_consumer_index		\
		pci_fast_registers->emc_ctd_v010_fregs_tx_index_consumer
#define response_producer_index		\
		pci_fast_registers->emc_ctd_v010_fregs_rx_index_producer
#define response_consumer_index		\
		pci_fast_registers->emc_ctd_v010_fregs_rx_index_consumer
#define pci_device_reset_register	\
		pci_slow_registers->emc_ctd_v010_sregs_reset

#define pci_device_name_bytes		\
	pci_fast_registers->emc_ctd_v010_fregs_device_name.emc_ctd_v010_name_bytes

struct ctd_event_io_element {
	struct list_head		list;
	union emc_ctd_v010_message	io_msg;
};

static inline unsigned long long ctd_read_tsc(void)
{
	unsigned long long local_tsc;

	local_tsc = rdtsc();

	return local_tsc;
}

#endif /* _EMCCTD_H_ */

/* vi: set ts=8 sw=8 noet: */
