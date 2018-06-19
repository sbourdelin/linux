/* SPDX-License-Identifier: GPL-2.0 */

/*
 * This file contains definitions from the Hyper-V Hypervisor Top-Level
 * Functional Specification (TLFS):
 * https://docs.microsoft.com/en-us/virtualization/hyper-v-on-windows/reference/tlfs
 *
 * Copyright (C) 2018, Microsoft, Inc.
 *
 * Author : Michael Kelley <mikelley@microsoft.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 */

#ifndef _ASM_ARM64_HYPERV_H
#define _ASM_ARM64_HYPERV_H

#include <linux/types.h>

/*
 * These Hyper-V registers provide information equivalent to the CPUID
 * instruction on x86/x64.
 */
#define HvRegisterHypervisorVersion		0x00000100 /*CPUID 0x40000002 */
#define	HvRegisterPrivilegesAndFeaturesInfo	0x00000200 /*CPUID 0x40000003 */
#define	HvRegisterFeaturesInfo			0x00000201 /*CPUID 0x40000004 */
#define	HvRegisterImplementationLimitsInfo	0x00000202 /*CPUID 0x40000005 */
#define HvARM64RegisterInterfaceVersion		0x00090006 /*CPUID 0x40000001 */

/*
 * Feature identification. HvRegisterPrivilegesAndFeaturesInfo returns a
 * 128-bit value with flags indicating which features are available to the
 * partition based upon the current partition privileges. The 128-bit
 * value is broken up with different portions stored in different 32-bit
 * fields in the ms_hyperv structure.
 */

/* Partition Reference Counter available*/
#define HV_MSR_TIME_REF_COUNT_AVAILABLE		(1 << 1)

/*
 * Synthetic Timers available
 */
#define HV_MSR_SYNTIMER_AVAILABLE		(1 << 3)

/* Frequency MSRs available */
#define HV_FEATURE_FREQUENCY_MSRS_AVAILABLE	(1 << 8)

/* Reference TSC available */
#define HV_MSR_REFERENCE_TSC_AVAILABLE		(1 << 9)

/* Crash MSR available */
#define HV_FEATURE_GUEST_CRASH_MSR_AVAILABLE	(1 << 10)


/*
 * This group of flags is in the high order 64-bits of the returned
 * 128-bit value.
 */

/* STIMER direct mode is available */
#define HV_STIMER_DIRECT_MODE_AVAILABLE		(1 << 19)

/*
 * Implementation recommendations in register
 * HvRegisterFeaturesInfo. Indicates which behaviors the hypervisor
 * recommends the OS implement for optimal performance.
 */

/*
 * Recommend not using Auto EOI
 */
#define HV_DEPRECATING_AEOI_RECOMMENDED		(1 << 9)

/*
 * Temporary #defines for compatibility with architecture
 * independent Hyper-V drivers. Remove these once x86-isms
 * have been removed from arch independent drivers.
 */
#define HV_X64_MSR_SYNTIMER_AVAILABLE \
		HV_MSR_SYNTIMER_AVAILABLE
#define HV_X64_STIMER_DIRECT_MODE_AVAILABLE \
		HV_STIMER_DIRECT_MODE_AVAILABLE
#define HV_X64_DEPRECATING_AEOI_RECOMMENDED \
		HV_DEPRECATING_AEOI_RECOMMENDED
#define HV_X64_MSR_STIMER0_COUNT 0
#define HV_X64_MSR_STIMER0_CONFIG 0
#define HV_X64_MSR_SINT0 0

/*
 * Synthetic register definitions equivalent to MSRs on x86/x64
 */
#define HvRegisterCrashP0       0x00000210
#define HvRegisterCrashP1       0x00000211
#define HvRegisterCrashP2       0x00000212
#define HvRegisterCrashP3       0x00000213
#define HvRegisterCrashP4       0x00000214
#define HvRegisterCrashCtl      0x00000215

#define HvRegisterGuestOsId     0x00090002
#define HvRegisterVpIndex       0x00090003
#define HvRegisterTimeRefCount  0x00090004
#define HvRegisterReferenceTsc	0x00090017

#define HvRegisterSint0         0x000A0000
#define HvRegisterSint1         0x000A0001
#define HvRegisterSint2         0x000A0002
#define HvRegisterSint3         0x000A0003
#define HvRegisterSint4         0x000A0004
#define HvRegisterSint5         0x000A0005
#define HvRegisterSint6         0x000A0006
#define HvRegisterSint7         0x000A0007
#define HvRegisterSint8         0x000A0008
#define HvRegisterSint9         0x000A0009
#define HvRegisterSint10        0x000A000A
#define HvRegisterSint11        0x000A000B
#define HvRegisterSint12        0x000A000C
#define HvRegisterSint13        0x000A000D
#define HvRegisterSint14        0x000A000E
#define HvRegisterSint15        0x000A000F
#define HvRegisterScontrol      0x000A0010
#define HvRegisterSversion      0x000A0011
#define HvRegisterSifp          0x000A0012
#define HvRegisterSipp          0x000A0013
#define HvRegisterEom           0x000A0014
#define HvRegisterSirbp         0x000A0015

#define HvRegisterStimer0Config 0x000B0000
#define HvRegisterStimer0Count  0x000B0001
#define HvRegisterStimer1Config 0x000B0002
#define HvRegisterStimer1Count  0x000B0003
#define HvRegisterStimer2Config 0x000B0004
#define HvRegisterStimer2Count  0x000B0005
#define HvRegisterStimer3Config 0x000B0006
#define HvRegisterStimer3Count  0x000B0007

/*
 * Crash notification flag used in the
 * CrashCtl register.
 */
#define HV_CRASH_CTL_CRASH_NOTIFY (1ULL << 63)

/*
 * The guest OS needs to register the guest ID with the hypervisor.
 * The guest ID is a 64 bit entity and the structure of this ID is
 * specified in the Hyper-V specification:
 *
 * msdn.microsoft.com/en-us/library/windows/hardware/ff542653%28v=vs.85%29.aspx
 *
 * While the current guideline does not specify how Linux guest ID(s)
 * need to be generated, our plan is to publish the guidelines for
 * Linux and other guest operating systems that currently are hosted
 * on Hyper-V. The implementation here conforms to this yet
 * unpublished guidelines.
 *
 *
 * Bit(s)
 * 63 - Indicates if the OS is Open Source or not; 1 is Open Source
 * 62:56 - Os Type; Linux is 0x100
 * 55:48 - Distro specific identification
 * 47:16 - Linux kernel version number
 * 15:0  - Distro specific identification
 *
 *
 */
#define HV_LINUX_VENDOR_ID              0x8100

/* Declare the various hypercall operations. */
#define HVCALL_FLUSH_VIRTUAL_ADDRESS_SPACE	0x0002
#define HVCALL_FLUSH_VIRTUAL_ADDRESS_LIST	0x0003
#define HVCALL_NOTIFY_LONG_SPIN_WAIT		0x0008
#define HVCALL_SEND_IPI				0x000b
#define HVCALL_FLUSH_VIRTUAL_ADDRESS_SPACE_EX	0x0013
#define HVCALL_FLUSH_VIRTUAL_ADDRESS_LIST_EX	0x0014
#define HVCALL_SEND_IPI_EX			0x0015
#define HVCALL_GET_VP_REGISTERS			0x0050
#define HVCALL_SET_VP_REGISTERS			0x0051
#define HVCALL_POST_MESSAGE			0x005c
#define HVCALL_SIGNAL_EVENT			0x005d
#define HVCALL_RETARGET_INTERRUPT		0x007e
#define HVCALL_START_VIRTUAL_PROCESSOR		0x0099
#define HVCALL_GET_VP_INDEX_FROM_APICID		0x009a

/* Declare standard hypercall field values. */
#define HV_PARTITION_ID_SELF                    ((u64)-1)
#define HV_VP_INDEX_SELF                        ((u32)-2)

#define HV_HYPERCALL_FAST_BIT                   BIT(16)
#define HV_HYPERCALL_REP_COUNT_1                BIT_ULL(32)
#define HV_HYPERCALL_RESULT_MASK                GENMASK_ULL(15, 0)

/* Define the hypercall status result */

union hv_hypercall_status {
	u64 as_uint64;
	struct {
		u16 status;
		u16 reserved;
		u16 reps_completed;  /* Low 12 bits */
		u16 reserved2;
	};
};

/* hypercall status code */
#define HV_STATUS_SUCCESS			0
#define HV_STATUS_INVALID_HYPERCALL_CODE	2
#define HV_STATUS_INVALID_HYPERCALL_INPUT	3
#define HV_STATUS_INVALID_ALIGNMENT		4
#define HV_STATUS_INSUFFICIENT_MEMORY		11
#define HV_STATUS_INVALID_CONNECTION_ID		18
#define HV_STATUS_INSUFFICIENT_BUFFERS		19

/* Define output layout for Get VP Register hypercall */
struct hv_get_vp_register_output {
	u64 registervaluelow;
	u64 registervaluehigh;
};

#define HV_FLUSH_ALL_PROCESSORS			BIT(0)
#define HV_FLUSH_ALL_VIRTUAL_ADDRESS_SPACES	BIT(1)
#define HV_FLUSH_NON_GLOBAL_MAPPINGS_ONLY	BIT(2)
#define HV_FLUSH_USE_EXTENDED_RANGE_FORMAT	BIT(3)

enum HV_GENERIC_SET_FORMAT {
	HV_GENERIC_SET_SPARSE_4K,
	HV_GENERIC_SET_ALL,
};

/*
 * The Hyper-V TimeRefCount register and the TSC
 * page provide a guest VM clock with 100ns tick rate
 */
#define HV_CLOCK_HZ (NSEC_PER_SEC/100)

/*
 * The fields in this structure are set by Hyper-V and read
 * by the Linux guest.  They should be accessed with READ_ONCE()
 * so the compiler doesn't optimize in a way that will cause
 * problems.
 */
struct ms_hyperv_tsc_page {
	u32 tsc_sequence;
	u32 reserved1;
	u64 tsc_scale;
	s64 tsc_offset;
	u64 reserved2[509];
};

/* Define the number of synthetic interrupt sources. */
#define HV_SYNIC_SINT_COUNT		(16)
/* Define the expected SynIC version. */
#define HV_SYNIC_VERSION_1		(0x1)

#define HV_SYNIC_CONTROL_ENABLE		(1ULL << 0)
#define HV_SYNIC_SIMP_ENABLE		(1ULL << 0)
#define HV_SYNIC_SIEFP_ENABLE		(1ULL << 0)
#define HV_SYNIC_SINT_MASKED		(1ULL << 16)
#define HV_SYNIC_SINT_AUTO_EOI		(1ULL << 17)
#define HV_SYNIC_SINT_VECTOR_MASK	(0xFF)

#define HV_SYNIC_STIMER_COUNT		(4)

/* Define synthetic interrupt controller message constants. */
#define HV_MESSAGE_SIZE			(256)
#define HV_MESSAGE_PAYLOAD_BYTE_COUNT	(240)
#define HV_MESSAGE_PAYLOAD_QWORD_COUNT	(30)

/* Define hypervisor message types. */
enum hv_message_type {
	HVMSG_NONE			= 0x00000000,

	/* Memory access messages. */
	HVMSG_UNMAPPED_GPA		= 0x80000000,
	HVMSG_GPA_INTERCEPT		= 0x80000001,

	/* Timer notification messages. */
	HVMSG_TIMER_EXPIRED		= 0x80000010,

	/* Error messages. */
	HVMSG_INVALID_VP_REGISTER_VALUE	= 0x80000020,
	HVMSG_UNRECOVERABLE_EXCEPTION	= 0x80000021,
	HVMSG_UNSUPPORTED_FEATURE	= 0x80000022,

	/* Trace buffer complete messages. */
	HVMSG_EVENTLOG_BUFFERCOMPLETE	= 0x80000040,
};

/* Define synthetic interrupt controller message flags. */
union hv_message_flags {
	__u8 asu8;
	struct {
		__u8 msg_pending:1;
		__u8 reserved:7;
	};
};

/* Define port identifier type. */
union hv_port_id {
	__u32 asu32;
	struct {
		__u32 id:24;
		__u32 reserved:8;
	} u;
};

/* Define synthetic interrupt controller message header. */
struct hv_message_header {
	__u32 message_type;
	__u8 payload_size;
	union hv_message_flags message_flags;
	__u8 reserved[2];
	union {
		__u64 sender;
		union hv_port_id port;
	};
};

/* Define synthetic interrupt controller message format. */
struct hv_message {
	struct hv_message_header header;
	union {
		__u64 payload[HV_MESSAGE_PAYLOAD_QWORD_COUNT];
	} u;
};

/* Define the synthetic interrupt message page layout. */
struct hv_message_page {
	struct hv_message sint_message[HV_SYNIC_SINT_COUNT];
};

/* Define timer message payload structure. */
struct hv_timer_message_payload {
	__u32 timer_index;
	__u32 reserved;
	__u64 expiration_time;	/* When the timer expired */
	__u64 delivery_time;	/* When the message was delivered */
};

#define HV_STIMER_ENABLE		(1ULL << 0)
#define HV_STIMER_PERIODIC		(1ULL << 1)
#define HV_STIMER_LAZY			(1ULL << 2)
#define HV_STIMER_AUTOENABLE		(1ULL << 3)
#define HV_STIMER_SINT(config)		(__u8)(((config) >> 16) & 0x0F)

#endif
