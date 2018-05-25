/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2018 Intel Corporation
 */

#ifndef _INTEL_GUC_CLIENT_INTERFACE_H_
#define _INTEL_GUC_CLIENT_INTERFACE_H_

#pragma pack(1)

/*****************************************************************************
 ********************************** Engines **********************************
 *****************************************************************************/

#define GUC_MAX_ENGINE_INSTANCE_PER_CLASS	4
#define GUC_MAX_SCHEDULABLE_ENGINE_CLASS	5
#define GUC_MAX_ENGINE_CLASS_COUNT		6
#define GUC_ENGINE_INVALID			6

/* Engine Class that uKernel can schedule on. This is just a SW enumeration.
 * HW configuration will depend on the Platform and SKU
 */
enum uk_engine_class {
	UK_RENDER_ENGINE_CLASS = 0,
	UK_VDECENC_ENGINE_CLASS = 1,
	UK_VE_ENGINE_CLASS = 2,
	UK_BLT_COPY_ENGINE_CLASS = 3,
	UK_RESERVED_ENGINE_CLASS = 4,
	UK_OTHER_ENGINE_CLASS = 5,
};

/* Engine Instance that uKernel can schedule on */
enum uk_engine_instance {
	UK_ENGINE_INSTANCE_0 = 0,
	UK_ENGINE_INSTANCE_1 = 1,
	UK_ENGINE_INSTANCE_2 = 2,
	UK_ENGINE_INSTANCE_3 = 3,
	UK_INVALID_ENGINE_INSTANCE = GUC_MAX_ENGINE_INSTANCE_PER_CLASS,
	UK_ENGINE_ALL_INSTANCES = UK_INVALID_ENGINE_INSTANCE
};

/* Target Engine field used in WI header and Guc2Host */
struct guc_target_engine {
	union {
		struct {
			/* One of enum uk_engine_class */
			u8 engine_class : 3;
			/* One of enum uk_engine_instance */
			u8 engine_instance : 4;
			/* All enabled engine classes and instances */
			u8 all_engines : 1;
		};
		u8 value;
	};
};

struct guc_engine_class_bit_map {
	union {
		/* Bit positions must match enum uk_engine_class value */
		struct {
			u32 render_engine_class : 1;
			u32 vdecenc_engine_class : 1;
			u32 ve_engine_class : 1;
			u32 blt_copy_engine_class : 1;
			u32 reserved_engine_class : 1;
			u32 other_engine_class : 1;
			u32 : 26;
		};
		u32 value;
	};
};

struct guc_engine_instance_bit_map {
	union {
		struct {
			u32 engine0 : 1;
			u32 engine1 : 1;
			u32 engine2 : 1;
			u32 engine3 : 1;
			u32 engine4 : 1;
			u32 engine5 : 1;
			u32 engine6 : 1;
			u32 engine7 : 1;
			u32 : 24;
		};
		u32 value;
	};
};

struct guc_engine_bit_map {
	struct guc_engine_class_bit_map engine_class_bit_map;
	struct guc_engine_instance_bit_map
		engine_instance_bit_map[GUC_MAX_ENGINE_CLASS_COUNT];
};

/*****************************************************************************
 ********************* Process Descriptor and Work Queue *********************
 *****************************************************************************/

/* Status of a Work Queue */
enum guc_queue_status {
	GUC_QUEUE_STATUS_ACTIVE = 1,
	GUC_QUEUE_STATUS_SUSPENDED = 2,
	GUC_QUEUE_STATUS_CMD_ERROR = 3,
	GUC_QUEUE_STATUS_ENGINE_ID_NOT_USED = 4,
	GUC_QUEUE_STATUS_SUSPENDED_FROM_ENGINE_RESET = 5,
	GUC_QUEUE_STATUS_INVALID_STATUS = 6,
};

/* Priority of guc_context_descriptor */
enum guc_context_priority {
	GUC_CONTEXT_PRIORITY_KMD_HIGH = 0,
	GUC_CONTEXT_PRIORITY_HIGH = 1,
	GUC_CONTEXT_PRIORITY_KMD_NORMAL = 2,
	GUC_CONTEXT_PRIORITY_NORMAL = 3,
	GUC_CONTEXT_PRIORITY_ABSOLUTE_MAX_COUNT = 4,
	GUC_CONTEXT_PRIORITY_INVALID = GUC_CONTEXT_PRIORITY_ABSOLUTE_MAX_COUNT
};

/* A shared structure between app and uKernel for communication */
struct guc_sched_process_descriptor {
	/* Index in the GuC Context Descriptor Pool */
	u32 context_id;

	/* Pointer to doorbell cacheline. BSpec: 1116 */
	u64 p_doorbell;

	/* WQ Head Byte Offset - Client must not write here */
	u32 head_offset;

	/* WQ Tail Byte Offset - uKernel will not write here */
	u32 tail_offset;

	/* WQ Error Byte offset */
	u32 error_offset_byte;

	/* WQ pVirt base address in Client. For use only by Client */
	u64 wqv_base_address;

	/* WQ Size in Bytes */
	u32 wq_size_bytes;

	/* WQ Status. Read by Client. Written by uKernel/KMD */
	enum guc_queue_status wq_status;

	/* Context priority. Read only by Client */
	enum guc_context_priority priority_assigned;

	u32 future;

	struct guc_engine_class_bit_map queue_engine_error;

	u32 reserved0[3];

	/* uKernel side tracking for debug */

	/* Written by uKernel at the time of parsing and successful removal
	 * from WQ (implies ring tail was updated)
	 */
	u32 total_work_items_parsed_by_guc;

	/* Written by uKernel if a WI was collapsed if next WI is the same
	 * LRCA (optimization applies only to Secure/KMD contexts)
	 */
	u32 total_work_items_collapsed_by_guc;

	/* Tells if the context is affected by Engine Reset. UMD needs to
	 * clear it after taking appropriate Action (TBD)
	 */
	u32 is_context_in_engine_reset;

	/* WQ Sampled tail at Engine Reset Time. Valid only if
	 * is_context_in_engine_reset = true
	 */
	u32 engine_reset_sampled_wq_tail;

	/* Valid from engine reset until all the affected Work Items are
	 * processed
	 */
	u32 engine_reset_sampled_wq_tail_valid;

	u32 reserved1[15];
};

/* Work Item for submitting KMD workloads into Work Queue for GuC */
struct guc_sched_work_queue_kmd_element_info {
	/* Execlist context descriptor's lower DW. BSpec: 12254 */
	u32 element_low_dw;
	union {
		struct {
			/* SW Context ID. BSpec: 12254 */
			u32 sw_context_index : 11;
			/* SW Counter. BSpec: 12254 */
			u32 sw_context_counter : 6;
			/* If this workload needs to be synced prior
			 * to submission use context_submit_sync_value and
			 * context_submit_sync_address
			 */
			u32 needs_sync : 1;
			/* QW Aligned, TailValue <= 2048
			 * (addresses 4 pages max)
			 */
			u32 ring_tail_qw_index : 11;
			u32 : 2;
			/* Bit to indicate if POCS needs to be in FREEZE state
			 * for this WI submission
			 */
			u32 wi_freeze_pocs : 1;
		};
		u32 value;
	} element_high_dw;
};

/* Work Item instruction type */
enum guc_sched_instruction_type {
	GUC_SCHED_INSTRUCTION_BATCH_BUFFER_START = 0x1,
	GUC_SCHED_INSTRUCTION_GUC_CMD_PSEUDO = 0x2,
	GUC_SCHED_INSTRUCTION_GUC_CMD_KMD = 0x3,
	GUC_SCHED_INSTRUCTION_GUC_CMD_NOOP = 0x4,
	GUC_SCHED_INSTRUCTION_RESUME_ENGINE_WQ_PARSING = 0x5,
	GUC_SCHED_INSTRUCTION_INVALID = 0x6,
};

/* Header for every Work Item put in the Work Queue */
struct guc_sched_work_queue_item_header {
	union {
		struct {
			/* One of enum guc_sched_instruction_type */
			u32 work_instruction_type : 8;
			/* struct guc_target_engine */
			u32 target_engine : 8;
			/* Length in number of dwords following this header */
			u32 command_length_dwords : 11;
			u32 : 5;
		};
		u32 value;
	};
};

/* Work item for submitting KMD workloads into work queue for GuC */
struct guc_sched_work_queue_item {
	struct guc_sched_work_queue_item_header header;
	struct guc_sched_work_queue_kmd_element_info kmd_submit_element_info;
	/* Debug only */
	u32 fence_id;
};

struct km_gen11_resume_work_queue_processing_item {
	struct guc_sched_work_queue_item header;
};

#pragma pack()

#endif
