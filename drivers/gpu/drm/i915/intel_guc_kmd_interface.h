/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2018 Intel Corporation
 */

#ifndef _INTEL_GUC_KMD_INTERFACE_H_
#define _INTEL_GUC_KMD_INTERFACE_H_

#include "intel_guc_client_interface.h"

#pragma pack(1)

/* Maximum number of entries in the GuC Context Descriptor Pool. Upper limit
 * restricted by number of 'SW Context ID' bits in the Context Descriptor
 * (BSpec: 12254) minus some reserved entries
 */
#define GUC_MAX_GUC_CONTEXT_DESCRIPTOR_ENTRIES	2032

/* Limited by 'SW Counter' bits. BSpec: 12254 */
#define GUC_MAX_SW_CONTEXT_COUNTER	64

/* Maximum depth of HW Execlist Submission Queue. BSpec: 18934 */
#define GUC_MAX_SUBMISSION_Q_DEPTH	8

/* Minimum depth of HW Execlist Submission Queue. BSpec: 18934 */
#define GUC_MIN_SUBMISSION_Q_DEPTH	2

/* Default depth of HW Execlist Submission Queue. BSpec: 18934 */
#define GUC_DEFAULT_ELEM_IN_SUBMISSION_Q	GUC_MIN_SUBMISSION_Q_DEPTH

/* 1 Cacheline = 64 Bytes */
#define GUC_DMA_CACHELINE_SIZE_BYTES	64

/*****************************************************************************
 ************************** Engines and System Info **************************
 *****************************************************************************/

/* GT system info passed down by KMD after reading fuse registers */
struct guc_gt_system_info {
	u32 slice_enabled;
	u32 rcs_enabled;
	u32 future0;
	u32 bcs_enabled;
	u32 vd_box_enable_mask;
	u32 future1;
	u32 ve_box_enable_mask;
	u32 future2;
	u32 reserved[8];
};

/*****************************************************************************
 ************************ GuC Context Descriptor Pool ************************
 *****************************************************************************/

/* State of the context */
struct guc_engine_context_state {
	union {
		struct {
			u32 wait_for_display_event : 1;
			u32 wait_for_semaphore : 1;
			u32 re_enqueue_to_submit_queue : 1;
			u32 : 29;
		};
		u32 wait_value;
	};
	u32 reserved;
};

/* To describe status and access information of current ring buffer for
 * a given guc_execlist_context
 */
struct guc_execlist_ring_buffer {
	u32 p_execlist_ring_context;

	/* uKernel address for the ring buffer */
	u32 p_ring_begin;
	/* uKernel final byte address that is valid for this ring */
	u32 p_ring_end;
	/* uKernel address for next location in ring */
	u32 p_next_free_location;

	/* Last value written by software for tracking (just in case
	 * HW corrupts the tail in its context)
	 */
	u32 current_tail_pointer_value;
};

/* The entire execlist context including software and HW information */
struct guc_execlist_context {
	/* 2 DWs of Context Descriptor. BSpec: 12254 */
	u32 hw_context_desc_dw[2];
	u32 reserved0;

	struct guc_execlist_ring_buffer ring_buffer_obj;
	struct guc_engine_context_state state;

	/* Flag to track if execlist context exists in submit queue
	 * Valid values 0 or 1
	 */
	u32 is_present_in_sq;

	/* If needs_sync is set in WI, sync *context_submit_sync_address ==
	 * context_submit_sync_value before submitting the context to HW
	 */
	u32 context_submit_sync_value;
	u32 context_submit_sync_address;

	/* Reserved for SLPC hints (currently used for GT throttle modes) */
	u32 slpc_context_hints;

	u32 reserved1[4];
};

/* Bitmap to track allocated and free contexts
 * context_alloct_bit_map[n] = 0; Context 'n' free
 * context_alloct_bit_map[n] = 1; Context 'n' allocated
 */
struct guc_execlist_context_alloc_map {
	/* Bit map for execlist contexts, bits 0 to
	 * (GUC_MAX_SW_CONTEXT_COUNTER - 1) are valid
	 */
	u64 context_alloct_bit_map;
	u32 reserved;
};

enum guc_context_descriptor_type {
	/* Work will be submitted through doorbell and WQ of a
	 * Proxy Submission descriptor in the context desc pool
	 */
	GUC_CONTEXT_DESCRIPTOR_TYPE_PROXY_ENTRY = 0x00,

	/* Work will be submitted using doorbell and workqueue
	 * of this descriptor on behalf of other proxy Entries
	 * in the context desc pool
	 */
	GUC_CONTEXT_DESCRIPTOR_TYPE_PROXY_SUBMISSION = 0x01,

	/* Work is submitted through its own doorbell and WQ */
	GUC_CONTEXT_DESCRIPTOR_TYPE_REAL = 0x02,
};

/* CPU, Graphics and physical addresses */
struct guc_address {
	/* Cpu address (virtual) */
	u64 p_cpu_address;
	/* uKernel address (gfx) */
	u32 p_uk_address;
	/* Physical address */
	u64 p_address_gpa;
};

/* Context descriptor for communication between uKernel and KMD */
struct guc_context_descriptor {
	/* CPU back pointer for general KMD usage */
	u64 assigned_guc_gpu_desc;

	/* Index in the pool */
	u32 guc_context_desc_pool_index;

	/* For a Proxy Entry, this is the index of it's proxy submission entry.
	 * For others this is the same as guc_context_desc_pool_index above
	 */
	u32 proxy_submission_guc_context_desc_pool_index;

	/* The doorbell page's trigger cacheline */
	struct guc_address doorbell_trigger_address;

	/* Assigned doorbell */
	u32 doorbell_id;

	/* Array of execlist contexts */
	struct guc_execlist_context
		uk_exec_list_context[GUC_MAX_SCHEDULABLE_ENGINE_CLASS]
				    [GUC_MAX_SW_CONTEXT_COUNTER];

	/* Allocation map to track which execlist contexts are in use */
	struct guc_execlist_context_alloc_map
		uk_execlist_context_alloc_map[GUC_MAX_SCHEDULABLE_ENGINE_CLASS];

	/* Number of active execlist contexts */
	u32 uk_execlist_context_alloc_count;

	/* Optimization to reduce the maximum execlist context count for
	 * this GuC context descriptor. Should be less than
	 * GUC_MAX_SW_CONTEXT_COUNTER
	 */
	u32 max_uk_execlist_context_per_engine_class;

	union {
		struct {
			/* Is this context actively assigned to an app? */
			u32 is_context_active : 1;

			/* Is this a proxy entry, principal or real entry? */
			u32 context_type : 2;

			u32 is_kmd_created_context : 1;

			/* Context was part of an engine reset. KMD must take
			 * appropriate action (this context will not be
			 * resubmitted until this bit is cleared)
			 */
			u32 is_context_eng_reset : 1;

			 /* Set it to 1 to prevent other code paths to do work
			  * queue processing as we use sampled values for WQ
			  * processing. Allowing multiple code paths to do WQ
			  * processing will cause same workload to execute
			  * multiple times
			  */
			u32 wq_processing_locked : 1;

			u32 future : 1;

			/* If set to 1, the context is terminated by GuC. All
			 * the pending work is dropped, its doorbell is evicted
			 * and eventually this context will be removed
			 */
			u32 is_context_terminated : 1;

			u32 : 24;
		};
		u32 bool_values;
	};

	enum guc_context_priority priority;

	/* WQ tail Sampled and set during doorbell ISR handler */
	u32 wq_sampled_tail_offset;

	/* Global (across all submit queues). For principals
	 * (proxy entry), this will be zero and true count
	 * will be reflected in its proxy (proxy submission)
	 */
	u32 total_submit_queue_enqueues;

	/* Pointer to struct guc_sched_process_descriptor */
	u32 p_process_descriptor;

	/* Secure copy of WQ address and size. uKernel can not
	 * trust data in struct guc_sched_process_descriptor
	 */
	u32 p_work_queue_address;
	u32 work_queue_size_bytes;

	u32 future0;
	u32 future1;

	struct guc_engine_class_bit_map queue_engine_error;

	u32 reserved0[3];
	u64 reserved1[12];
};

/*****************************************************************************
 *************************** Host2GuC and GuC2Host ***************************
 *****************************************************************************/

/* Host 2 GuC actions */
enum guc_host2guc_action {
	GUC_HOST2GUC_ACTION_DEFAULT = 0x0,
	GUC_HOST2GUC_ACTION_REQUEST_INIT_DONE_INTERRUPT = 0x1,
	GUC_HOST2GUC_ACTION_REQUEST_PREEMPTION = 0x2,
	GUC_HOST2GUC_ACTION_REQUEST_ENGINE_RESET = 0x3,
	GUC_HOST2GUC_ACTION_PAUSE_SCHEDULING = 0x4,
	GUC_HOST2GUC_ACTION_RESUME_SCHEDULING = 0x5,

	GUC_HOST2GUC_ACTION_ALLOCATE_DOORBELL = 0x10,
	GUC_HOST2GUC_ACTION_DEALLOCATE_DOORBELL = 0x20,
	GUC_HOST2GUC_ACTION_LOG_BUFFER_FILE_FLUSH_COMPLETE = 0x30,
	GUC_HOST2GUC_ACTION_ENABLE_LOGGING = 0x40,
	GUC_HOST2GUC_ACTION_CACHE_CRASH_DUMP = 0x200,
	GUC_HOST2GUC_ACTION_DEBUG_RING_DB = 0x300,
	GUC_HOST2GUC_ACTION_PERFORM_GLOBAL_DEBUG_ACTIONS = 0x301,
	GUC_HOST2GUC_ACTION_FORCE_LOGBUFFERFLUSH = 0x302,
	GUC_HOST2GUC_ACTION_LOG_VERBOSITY_LOGOUTPUT_SELECT = 0x400,
	GUC_HOST2GUC_ACTION_ENTER_S_STATE = 0x501,
	GUC_HOST2GUC_ACTION_EXIT_S_STATE = 0x502,
	GUC_HOST2GUC_ACTION_SET_SCHEDULING_MODE = 0x504,
	GUC_HOST2GUC_ACTION_SCHED_POLICY_CHANGE = 0x506,

	/* Actions for Powr Conservation : 0x3000-0x3FFF */
	GUC_HOST2GUC_ACTION_PC_SLPM_REQUEST = 0x3003,
	GUC_HOST2GUC_ACTION_PC_SETUP_GUCRC = 0x3004,
	GUC_HOST2GUC_ACTION_SAMPLE_FORCEWAKE_FEATURE_REGISTER = 0x3005,
	GUC_HOST2GUC_ACTION_SETUP_GUCRC = 0x3006,

	GUC_HOST2GUC_ACTION_AUTHENTICATE_HUC = 0x4000,

	GUC_HOST2GUC_ACTION_REGISTER_COMMAND_TRANSPORT_BUFFER = 0x4505,
	GUC_HOST2GUC_ACTION_DEREGISTER_COMMAND_TRANSPORT_BUFFER = 0x4506,

	GUC_HOST2GUC_ACTION_MAX = 0xFFFF
};

enum guc_host2guc_response_status {
	GUC_HOST2GUC_RESPONSE_STATUS_SUCCESS = 0x0,
	GUC_HOST2GUC_RESPONSE_STATUS_UNKNOWN_ACTION = 0x30,
	GUC_HOST2GUC_RESPONSE_STATUS_LOG_HOST_ADDRESS_NOT_VALID = 0x80,
	GUC_HOST2GUC_RESPONSE_STATUS_GENERIC_FAIL = 0xF000,
};

enum {
	/* Host originating types */
	GUC_MSG_TYPE_HOST2GUC_REQUEST = 0x0,

	/* GuC originating types */
	GUC_MSG_TYPE_HOST2GUC_RESPONSE = 0xF,
} GUC_GUC_MSG_TYPE;

/* This structure represents the various formats of values put in
 * SOFT_SCRATCH_0. The "Type" field is to determine which register
 * definition to use, so it must be common among all unioned
 * structs.
 */
struct guc_msg_format {
	union {
		struct {
			u32 action : 16; /* enum guc_host2guc_action */
			u32 reserved : 12; /* MBZ */
			u32 type : 4; /* GUC_MSG_TYPE_HOST2GUC_REQUEST */
		} host2guc_action;

		struct {
			u32 status : 16; /* enum guc_host2guc_response_status */
			u32 return_data : 12;
			u32 type : 4; /* GUC_MSG_TYPE_HOST2GUC_RESPONSE */
		} host2guc_response;

		u32 dword_value;
	};
};

#define GUC_MAKE_HOST2GUC_RESPONSE(_status, _return_data)	\
	((GUC_MSG_TYPE_HOST2GUC_RESPONSE << 28) |		\
	((_return_data & 0xFFF) << 16) |			\
	(_status & 0xFFFF))
#define GUC_MAKE_HOST2GUC_STATUS(a) (GUC_MAKE_HOST2GUC_RESPONSE(a, 0))

enum guc_cmd_transport_buffer_type {
	GUC_CMD_TRANSPORT_BUFFER_HOST2GUC = 0x00,
	GUC_CMD_TRANSPORT_BUFFER_GUC2HOST = 0x01,
	GUC_CMD_TRANSPORT_BUFFER_MAX_TYPE = 0x02,
};

struct guc_cmd_transport_buffer_desc {
	u32 buffer_begin_gfx_address;
	u64 buffer_begin_virtual_address;
	u32 buffer_size_in_bytes;
	/* GuC uKernel updates this */
	u32 head_offset;
	/* GuC client updates this */
	u32 tail_offset;
	u32 is_in_error;
	/* A DW provided by H2G item that was requested to be written */
	u32 fence_report_dw;
	/* Status associated with above fence_report_dw */
	u32 status_report_dw;
	/* ID associated with this buffer (assigned by GuC master) */
	u32 client_id;
	/* Used & set by the client for further tracking of internal clients */
	u32 client_sub_tracking_id;
	u32 reserved[5];
};

/* Per client command transport buffer allocated by GuC master */
struct guc_master_cmd_transport_buffer_alloc {
	/* This is the copy that GuC trusts */
	struct guc_cmd_transport_buffer_desc buffer_desc;
	u32 future;
	u64 reserved0;
	u32 usage_special_info;
	u32 valid;
	u32 associated_g2h_index;
	u32 reserved1;
};

/*                             Host 2 GuC Work Item
 * V-----------------------------------------------------------------------V
 * *************************************************************************
 * *                   *    DW0/   *           *               *           *
 * * H2G Item Header   *  ReturnDW *  DW1      *      ...      *  DWn      *
 * *************************************************************************
 */

/* Command buffer header */
struct guc_cmd_buffer_item_header {
	union {
		struct {
			/* Number of dwords that are parameters of this
			 * Host2GuC action. Max of 31. E.g.: if there are 2 DWs
			 * following this header, this field is set to 2
			 */
			u32 num_dwords : 5;

			u32 : 3;

			/* The uKernel will write the value from DW0 (aka
			 * ReturnDW) to fence_report_dw in struct
			 * guc_cmd_transport_buffer_desc
			 */
			u32 write_fence_from_dw0_to_descriptor : 1;

			/* Write the status of the action to DW0 following this
			 * header
			 */
			u32 write_status_to_dw0 : 1;

			/* Send a GuC2Host with Status of the action and the
			 * fence ID found in DW0 via the buffer used for GuC to
			 * Host communication
			 */
			u32 send_status_with_dw0_via_guc_to_host : 1;

			u32 : 5;

			/*  This is the value of the enum guc_host2guc_action
			 * that needs to be done by the uKernel
			 */
			u32 host2guc_action : 16;
		} h2g_cmd_buffer_item_header;

		struct {
			/* Number of dwords that are parameters of this GuC2Host
			 * action
			 */
			u32 num_dwords : 5;

			u32 : 3;

			/* Indicates that this GuC2Host action is a response of
			 * a Host2Guc request
			 */
			u32 host2_guc_response : 1;

			u32 reserved : 7;

			/* struct guc_to_host_message */
			u32 guc2host_action : 16;
		} g2h_cmd_buffer_item_header;

		struct {
			u32 num_dwords : 5;
			u32 reserved : 3;
			u32 free_for_client_use : 24;
		} generic_cmd_buffer_item_header;

		u32 header_value;
	};

};

struct guc_to_host_message {
	union {
		struct {
			u32 uk_init_done : 1;
			u32 crash_dump_posted : 1;
			u32 : 1;
			u32 flush_log_buffer_to_file : 1;
			u32 preempt_request_old_preempt_pending : 1;
			u32 preempt_request_target_context_bad : 1;
			u32 : 1;
			u32 sleep_entry_in_progress : 1;
			u32 guc_in_debug_halt : 1;
			u32 guc_report_engine_reset_context_id : 1;
			u32 : 1;
			u32 host_preemption_complete : 1;
			u32 reserved0 : 4;
			u32 gpa_to_hpa_xlation_error : 1;
			u32 doorbell_id_allocation_error : 1;
			u32 doorbell_id_allocation_invalid_ctx_id : 1;
			u32 : 1;
			u32 force_wake_timed_out : 1;
			u32 force_wake_time_out_counter : 2;
			u32 : 1;
			u32 iommu_cat_page_faulted : 1;
			u32 host2guc_engine_reset_complete : 1;
			u32 reserved1 : 2;
			u32 doorbell_selection_error : 1;
			u32 doorbell_id_release_error : 1;
			u32 uk_exception : 1;
			u32 : 1;
		};
		u32 dw;
	};

};

/* Size of the buffer to save GuC's state before S3. The ddress of the buffer
 * goes in guc_additional_data_structs
 */
#define GUC_MAX_GUC_S3_SAVE_SPACE_PAGES	10

/* MMIO Offset for status of sleep state enter request */
#define GUC_SLEEP_STATE_ENTER_STATUS	0xC1B8

/* Status of sleep request. Value updated in GUC_SLEEP_STATE_ENTER_STATUS */
enum guc_sleep_state_enter_status {
	GUC_SLEEP_STATE_ENTER_SUCCESS = 1,
	GUC_SLEEP_STATE_ENTER_PREEMPT_TO_IDLE_FAILED = 2,
	GUC_SLEEP_STATE_ENTER_ENG_RESET_FAILED = 3,
};


/* Enum to determine what mode the scheduler is in */
enum guc_scheduler_mode {
	GUC_SCHEDULER_MODE_NORMAL,
	GUC_SCHEDULER_MODE_STALL_IMMEDIATE,
};

/*****************************************************************************
 ********************************** Logging **********************************
 *****************************************************************************/

enum guc_log_buffer_type {
	GUC_LOG_BUFFER_TYPE_ISR = 0x0,
	GUC_LOG_BUFFER_TYPE_DPC = 0x1,
	GUC_LOG_BUFFER_TYPE_CRASH = 0x2,
	GUC_LOG_BUFFER_TYPE_MAX = 0x3,
};

enum guc_log_verbosity {
	GUC_LOG_VERBOSITY_LOW = 0x0,
	GUC_LOG_VERBOSITY_MED = 0x1,
	GUC_LOG_VERBOSITY_HIGH = 0x2,
	GUC_LOG_VERBOSITY_ULTRA = 0x3,
	GUC_LOG_VERBOSITY_MAX = 0x4,
};

/* This enum controls the type of logging output. Can be changed dynamically
 * using GUC_HOST2GUC_ACTION_LOG_VERBOSITY_LOGOUTPUT_SELECT
 */
enum guc_logoutput_selection {
	GUC_LOGOUTPUT_LOGBUFFER_ONLY = 0x0,
	GUC_LOGOUTPUT_NPK_ONLY = 0x1,
	GUC_LOGOUTPUT_LOGBUFFER_AND_NPK = 0x2,
	GUC_LOGOUTPUT_MAX
};

/* Filled by KMD except version and marker that are initialized by uKernel */
struct guc_km_log_buffer_state {
	/* Marks the beginning of Buffer Flush (set by uKernel at Log Buffer
	 * Init)
	 */
	u32 marker[2];

	/* This is the last byte offset location that was read by KMD. KMD will
	 * write to this and uKernel will read it
	 */
	u32 log_buf_rd_ptr;

	/* This is the byte offset location that will be written by uKernel */
	u32 log_buf_wr_ptr;

	u32 log_buf_size;

	/* This is written by uKernel when it sees the log buffer becoming half
	 * full. KMD writes this value in the log file to avoid stale data
	 */
	u32 sampled_log_buf_wrptr;

	union {
		struct {
			/* uKernel sets this when log buffer is half full or
			 * when a forced flush has been requested through
			 * Host2Guc. uKernel will send Guc2Host only if this
			 * bit is cleared. This is to avoid unnecessary
			 * interrupts from GuC
			 */
			u32 log_buf_flush_to_file : 1;

			/* uKernel increments this when log buffer overflows */
			u32 buffer_full_count : 4;

			u32 : 27;
		};
		u32 log_buf_flags;
	};

	u32 version;
};

/* Logging Parameters sent via struct sched_control_data. Maintained as separate
 * structure to allow debug tools to access logs without contacting GuC (for
 * when GuC is stuck in ISR)
 */
struct guc_log_init_params {
	union {
		struct {
			u32 is_log_buffer_valid : 1;
			/* Raise GuC2Host interrupt when buffer is half full */
			u32 notify_on_log_half_full : 1;
			u32 : 1;
			/* 0 = Pages, 1 = Megabytes */
			u32 allocated_count_units : 1;
			/* No. of units allocated -1 (MAX 4 Units) */
			u32 crash_dump_log_allocated_count : 2;
			/* No. of units allocated -1 (MAX 8 Units) */
			u32 dpc_log_allocated_count : 3;
			/* No. of units allocated -1 (MAX 8 Units) */
			u32 isr_log_allocated_count : 3;
			/* Page aligned address for log buffer */
			u32 log_buffer_gfx_address : 20;
		};
		u32 log_dword_value;
	};
};

/* Pass info for doing a Host2GuC request (GUC_HOST2GUC_ACTION_ENABLE_LOGGING)
 * in order to enable/disable GuC logging
 */
struct guc_log_enable_params {
	union {
		struct {
			u32 logging_enabled : 1;
			u32 profile_logging_enabled : 1;
			u32 log_output_selection : 2;
			u32 log_verbosity : 4;
			u32 default_logging_enabled : 1;
			u32 : 23;
		};
		u32 log_enable_dword_value;
	};

};

/*****************************************************************************
 ************** Sched Control Data and Addtional Data Structures *************
 *****************************************************************************/

/* Holds the init values of various parameters used by the uKernel */
struct guc_sched_control_data {
	/* Dword 0 */
	union {
		struct {
			/* Num of contexts in pool in blocks of 16,
			 * E.g.: num_contexts_in_pool16_blocks = 1 if 16
			 * contexts, 64 if 1024 contexts allocated
			 */
			u32 num_contexts_in_pool16_blocks : 12;

			/* Aligned bits [31:12] of the GFX address where the
			 * pool begins
			 */
			u32 context_pool_gfx_address_begin : 20;
		};
	};

	/* Dword 1 */
	struct guc_log_init_params log_init_params;


	/* Dword 2 */
	union {
		struct {
			u32 reserved : 1;
			u32 wa_disable_dummy_all_engine_fault_fix : 1;
			u32 : 30;
		};
		u32 workaround_dw;
	};

	/* Dword 3 */
	union {
		struct {
			u32 ftr_enable_preemption_data_logging : 1;
			u32 ftr_enable_guc_pavp_control : 1;
			u32 ftr_enable_guc_slpm : 1;
			u32 ftr_enable_engine_reset_on_preempt_failure : 1;
			u32 ftr_lite_restore : 1;
			u32 ftr_driver_flr : 1;
			u32 future : 1;
			u32 ftr_enable_psmi_logging : 1;
			u32 : 1;
			u32 : 1;
			u32 : 1;
			u32 : 1;
			u32 : 1;
			u32 : 1;
			u32 : 18;
		};
		u32 feature_dword;
	};

	/* Dword 4 */
	union {
		struct {
			/* One of enum guc_log_verbosity */
			u32 logging_verbosity : 4;
			/* One of enum guc_logoutput_selection */
			u32 log_output_selection : 2;
			u32 logging_disabled : 1;
			u32 profile_logging_enabled : 1;
			u32 : 24;
		};
	};

	/* Dword 5 */
	union {
		struct {
			u32 : 1;
			u32 gfx_address_additional_data_structs : 21;
			u32 : 10;
		};
	};

};

/* Structure to pass additional information and structure pointers to */
struct guc_additional_data_structs {
	/* Gfx ptr to struct guc_mmio_save_restore_list (persistent) */
	u32 gfx_address_mmio_save_restore_list;

	/* Buffer of size GUC_MAX_GUC_S3_SAVE_SPACE_PAGES (persistent) */
	u32 gfx_ptr_to_gucs_state_save_buffer;

	/* Gfx addresses of struct guc_scheduling_policies (non-persistent, may
	 * be released after initial load), NULL or valid = 0 flag value will
	 * cause default policies to be loaded
	 */
	u32 gfx_scheduler_policies;

	/* Gfx address of struct guc_gt_system_info */
	u32 gt_system_info;

	u32 future;

	u32 gfx_ptr_to_psmi_log_control_data;

	/* LRCA addresses and sizes of golden contexts (persistent) */
	u32 gfx_golden_context_lrca[GUC_MAX_SCHEDULABLE_ENGINE_CLASS];
	u32 golden_context_eng_state_size_in_bytes[GUC_MAX_SCHEDULABLE_ENGINE_CLASS];

	u32 reserved[16];
};

/* Max number of mmio per engine class per engine instance */
#define GUC_MAX_MMIO_PER_SET	64

struct guc_mmio_flags {
	union {
		struct {
			u32 masked : 1;
			u32 : 31;
		};
		u32 flags_value;
	};
};

struct guc_mmio {
	u32 offset;
	u32 value;
	struct guc_mmio_flags flags;
};

struct guc_mmio_set {
	/* Array of mmio to be saved/restored */
	struct guc_mmio mmio[GUC_MAX_MMIO_PER_SET];
	/* Set after saving mmio value, cleared after restore. */
	u32 mmio_values_valid;
	 /* Number of mmio in the set */
	u32 number_of_mmio;
};

struct guc_mmio_save_restore_list {
	struct guc_mmio_set
		node_mmio_set[GUC_MAX_SCHEDULABLE_ENGINE_CLASS]
			     [GUC_MAX_ENGINE_INSTANCE_PER_CLASS];
	u32 reserved[98];
};

/* Policy flags to control scheduling decisions */
struct guc_scheduling_policy_flags {
	union {
		struct {
			/* Should we reset engine when preemption failed within
			 * its time quantum
			 */
			u32 reset_engine_upon_preempt_failure : 1;

			/* Should we preempt to idle unconditionally for the
			 * execution quantum expiry
			 */
			u32 preempt_to_idle_on_quantum_expiry : 1;

			u32 : 30;
		};
		u32 policy_dword;
	};
};

/* Per-engine class and per-priority struct for scheduling policy  */
struct guc_scheduling_policy {
	/* Time for one workload to execute (micro seconds) */
	u32 execution_quantum;

	/* Time to wait for a preemption request to completed before issuing a
	 * reset (micro seconds)
	 */
	u32 wait_for_preemption_completion_time;

	/* How much time to allow to run after the first fault is observed.
	 * Then preempt afterwards (micro seconds)
	 */
	u32 quantum_upon_first_fault_time;

	struct guc_scheduling_policy_flags policy_flags;

	u32 reserved[8];
};

/* KMD should populate this struct and pass info through struct
 * guc_additional_data_structs- If KMD does not set the scheduler policy,
 * uKernel will fall back to default scheduling policies
 */
struct guc_scheduling_policies {
	struct guc_scheduling_policy
		per_submit_queue_policy[GUC_CONTEXT_PRIORITY_ABSOLUTE_MAX_COUNT]
				       [GUC_MAX_SCHEDULABLE_ENGINE_CLASS];

	/* Submission queue depth, min 2, max 8. If outside the valid range,
	 * default value is used
	 */
	u32 submission_queue_depth[GUC_MAX_SCHEDULABLE_ENGINE_CLASS];

	/* How much time to allow before DPC processing is called back via
	 * interrupt (to prevent DPC queue drain starving) IN micro seconds.
	 * Typically in the 1000s (example only, not granularity)
	 */
	u32 dpc_promote_time;

	/* Must be set to take these new values */
	u32 is_valid;

	/* Number of WIs to process per call to process single. Process single
	 * could have a large Max Tail value which may keep CS idle. Process
	 * max_num_work_items_per_dpc_call WIs and try fast schedule
	 */
	u32 max_num_work_items_per_dpc_call;

	u32 reserved[4];
};

#pragma pack()

#endif
