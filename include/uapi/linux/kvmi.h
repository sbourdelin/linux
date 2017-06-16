/*
 * Copyright (C) 2017 Bitdefender S.R.L.
 *
 * The KVMI Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * The KVMI Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the GNU C Library; if not, see
 * <http://www.gnu.org/licenses/>
 */
#ifndef __KVMI_H_INCLUDED__
#define __KVMI_H_INCLUDED__

#include "asm/kvm.h"

#define KVMI_VERSION 0x00000001

#define KVMI_EVENT_CR         (1 << 1)	/* control register was modified */
#define KVMI_EVENT_MSR        (1 << 2)	/* model specific reg. was modified */
#define KVMI_EVENT_XSETBV     (1 << 3)	/* ext. control register was modified */
#define KVMI_EVENT_BREAKPOINT (1 << 4)	/* breakpoint was reached */
#define KVMI_EVENT_USER_CALL  (1 << 5)	/* user hypercall */
#define KVMI_EVENT_PAGE_FAULT (1 << 6)	/* hyp. page fault was encountered */
#define KVMI_EVENT_TRAP       (1 << 7)	/* trap was injected */
#define KVMI_EVENT_SET_CTX    (1 << 28)	/* set the emulation context */
#define KVMI_EVENT_NOEMU      (1 << 29)	/* return to guest without emulation */
#define KVMI_EVENT_SET_REGS   (1 << 30)	/* registers need to be written back */
#define KVMI_EVENT_ALLOW      (1 << 31)	/* used in replies */

#define KVMI_KNOWN_EVENTS (KVMI_EVENT_CR | \
			   KVMI_EVENT_MSR | \
			   KVMI_EVENT_XSETBV | \
			   KVMI_EVENT_BREAKPOINT | \
			   KVMI_EVENT_USER_CALL | \
			   KVMI_EVENT_PAGE_FAULT | \
			   KVMI_EVENT_TRAP)

#define KVMI_FLAG_RESPONSE 0x8000

#define KVMI_GET_VERSION                  1
#define KVMI_GET_GUESTS                   2
#define KVMI_GET_GUEST_INFO               3
#define KVMI_PAUSE_GUEST                  4
#define KVMI_UNPAUSE_GUEST                5
#define KVMI_GET_REGISTERS                6
#define KVMI_SET_REGISTERS                7
#define KVMI_SHUTDOWN_GUEST               8
#define KVMI_GET_MTRR_TYPE                9
#define KVMI_GET_MTRRS                    10
#define KVMI_GET_XSAVE_INFO               11
#define KVMI_GET_PAGE_ACCESS              12
#define KVMI_SET_PAGE_ACCESS              13
#define KVMI_INJECT_PAGE_FAULT            14
#define KVMI_READ_PHYSICAL                15
#define KVMI_WRITE_PHYSICAL               16
#define KVMI_MAP_PHYSICAL_PAGE_TO_SVA     17
#define KVMI_UNMAP_PHYSICAL_PAGE_FROM_SVA 18
#define KVMI_EVENT_CONTROL                19
#define KVMI_CR_CONTROL                   20
#define KVMI_MSR_CONTROL                  21
#define KVMI_INJECT_BREAKPOINT            22
#define KVMI_EVENT_GUEST_ON               23
#define KVMI_EVENT_GUEST_OFF              24
#define KVMI_EVENT_VCPU                   25
#define KVMI_REPLY_EVENT_VCPU             26

struct kvmi_socket_hdr {
	__u16 msg_id;
	__u16 size;
	__u32 seq;
};

struct kvmi_event_reply {
	struct kvm_regs regs;
	__u64 new_val;
	__u32 event;
	__u32 padding1;
	__u8 ctx_data[256];
	__u32 ctx_size;
	__u32 padding2;
};

struct kvmi_guest {
	__u8 uuid[16];
};

struct kvmi_guests {
	__u32 size;		/* in: the size of the entire structure */
	struct kvmi_guest guests[1];
};

struct kvmi_event_cr {
	__u16 cr;
	__u16 padding1;
	__u32 padding2;
	__u64 old_value;
	__u64 new_value;
};

struct kvmi_event_msr {
	__u32 msr;
	__u32 padding;
	__u64 old_value;
	__u64 new_value;
};

struct kvmi_event_xsetbv {
	__u64 xcr0;
};

struct kvmi_event_breakpoint {
	__u64 gpa;
};

struct kvmi_event_page_fault {
	__u64 gva;
	__u64 gpa;
	__u32 mode;
	__u32 padding;
};

struct kvmi_event_trap {
	__u32 vector;
	__u32 type;
	__u32 err;
	__u32 padding;
	__u64 cr2;
};

struct kvmi_event {
	__u16 vcpu;
	__u8 mode;		/* 2, 4 or 8 */
	__u8 padding1;
	__u32 event;
	struct kvm_regs regs;	/* in/out */
	struct kvm_sregs sregs;	/* in */
	struct {
		__u64 sysenter_cs;
		__u64 sysenter_esp;
		__u64 sysenter_eip;
		__u64 efer;
		__u64 star;
		__u64 lstar;
	} msrs;
	union {
		struct kvmi_event_cr cr;
		struct kvmi_event_msr msr;
		struct kvmi_event_xsetbv xsetbv;
		struct kvmi_event_breakpoint breakpoint;
		struct kvmi_event_page_fault page_fault;
		struct kvmi_event_trap trap;
	};			/* out */
};

struct kvmi_event_control {
	__u16 vcpu;
	__u16 padding;
	__u32 events;
};

struct kvmi_cr_control {
	__u8 enable;
	__u8 padding1;
	__u16 padding2;
	__u32 cr;
};

struct kvmi_msr_control {
	__u8 enable;
	__u8 padding1;
	__u16 padding2;
	__u32 msr;
};

struct kvmi_page_access {
	__u16 vcpu;
	__u16 padding;
	int err;
	__u64 gpa;
	__u64 access;
};

struct kvmi_mtrr_type {
	int err;
	__u32 padding;
	__u64 gpa;
	__u64 type;
};

struct kvmi_mtrrs {
	__u16 vcpu;
	__u16 padding;
	int err;
	__u64 pat;
	__u64 cap;
	__u64 type;
};

struct kvmi_guest_info {
	__u16 vcpu_count;
	__u16 padding1;
	__u32 padding2;
	__u64 tsc_speed;
};

struct kvmi_xsave_info {
	__u16 vcpu;
	__u16 padding;
	int err;
	__u64 size;
};

struct kvmi_page_fault {
	__u16 vcpu;
	__u16 padding;
	__u32 error;
	__u64 gva;
};

struct kvmi_rw_physical_info {
	__u64 gpa;
	__u64 buffer;
	__u64 size;
};

struct kvmi_map_physical_to_sva_info {
	__u64 gpa_src;
	__u64 gfn_dest;
};

struct kvmi_unmap_physical_from_sva_info {
	__u64 gfn_dest;
};

struct kvmi_get_registers {
	__u16 vcpu;
	__u16 nmsrs;
	__u32 msrs_idx[0];
};

struct kvmi_get_registers_r {
	int err;
	__u32 mode;
	struct kvm_regs regs;
	struct kvm_sregs sregs;
	struct kvm_msrs msrs;
};

struct kvmi_set_registers {
	__u16 vcpu;
	__u16 padding1;
	__u32 padding2;
	struct kvm_regs regs;
};

#endif /* __KVMI_H_INCLUDED__ */
