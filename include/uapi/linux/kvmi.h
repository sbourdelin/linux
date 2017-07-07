/*
 * Copyright (C) 2017 Bitdefender S.R.L.
 *
 * The KVMI Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the named License, or any later version.
 *
 * The KVMI Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with the KVMI Library; if not, see <http://www.gnu.org/licenses/>
 */
#ifndef __KVMI_H_INCLUDED__
#define __KVMI_H_INCLUDED__

#include "asm/kvm.h"
#include <linux/types.h>

#define KVMI_VERSION 0x00000001

#define KVMI_EVENT_CR         (1 << 1)	/* control register was modified */
#define KVMI_EVENT_MSR        (1 << 2)	/* model specific reg. was modified */
#define KVMI_EVENT_XSETBV     (1 << 3)	/* ext. control register was modified */
#define KVMI_EVENT_BREAKPOINT (1 << 4)	/* breakpoint was reached */
#define KVMI_EVENT_USER_CALL  (1 << 5)	/* user hypercall */
#define KVMI_EVENT_PAGE_FAULT (1 << 6)	/* hyp. page fault was encountered */
#define KVMI_EVENT_TRAP       (1 << 7)	/* trap was injected */

#define KVMI_KNOWN_EVENTS (KVMI_EVENT_CR | \
			   KVMI_EVENT_MSR | \
			   KVMI_EVENT_XSETBV | \
			   KVMI_EVENT_BREAKPOINT | \
			   KVMI_EVENT_USER_CALL | \
			   KVMI_EVENT_PAGE_FAULT | \
			   KVMI_EVENT_TRAP)

#define KVMI_EVENT_ACTION_ALLOW      (1 << 0)	/* used in replies */
#define KVMI_EVENT_ACTION_SET_REGS   (1 << 1)	/* registers need to be written back */
#define KVMI_EVENT_ACTION_SET_CTX    (1 << 2)	/* set the emulation context */
#define KVMI_EVENT_ACTION_NOEMU      (1 << 3)	/* return to guest without emulation */

#define KVMI_GET_VERSION                    1
#define KVMI_GET_GUESTS                     2 /* TODO: remove me */
#define KVMI_GET_GUEST_INFO                 3
#define KVMI_PAUSE_GUEST                    4
#define KVMI_UNPAUSE_GUEST                  5
#define KVMI_GET_REGISTERS                  6
#define KVMI_SET_REGISTERS                  7
#define KVMI_SHUTDOWN_GUEST                 8
#define KVMI_GET_MTRR_TYPE                  9
#define KVMI_GET_MTRRS                      10
#define KVMI_GET_XSAVE_INFO                 11
#define KVMI_GET_PAGE_ACCESS                12
#define KVMI_SET_PAGE_ACCESS                13
#define KVMI_INJECT_PAGE_FAULT              14
#define KVMI_READ_PHYSICAL                  15 /* TODO: remove me */
#define KVMI_WRITE_PHYSICAL                 16 /* TODO: remove me */
#define KVMI_MAP_PHYSICAL_PAGE_TO_GUEST     17
#define KVMI_UNMAP_PHYSICAL_PAGE_FROM_GUEST 18
#define KVMI_CONTROL_EVENTS                 19
#define KVMI_CR_CONTROL                     20
#define KVMI_MSR_CONTROL                    21
#define KVMI_INJECT_BREAKPOINT              22
#define KVMI_EVENT_GUEST_ON                 23 /* TODO: remove me */
#define KVMI_EVENT_GUEST_OFF                24 /* TODO: remove me */
#define KVMI_EVENT_VCPU                     25
#define KVMI_EVENT_VCPU_REPLY               26

/* TODO: remove me */
struct kvmi_guest {
	__u8 uuid[16];
};

/* TODO: remove me */
struct kvmi_guests {
	__u32 size;		/* in: the size of the entire structure */
	struct kvmi_guest guests[1];
};

/* TODO: remove me */
struct kvmi_read_physical {
	__u64 gpa;
	__u64 size;
};

/* TODO: remove me */
struct kvmi_read_physical_reply {
	__s32 err;
	__u8 bytes[0];
};

/* TODO: remove me */
struct kvmi_write_physical {
	__u64 gpa;
	__u64 size;
	__u8 bytes[0];
};


struct kvmi_socket_hdr {
	__u16 msg_id;
	__u16 size;
	__u32 seq;
};

struct kvmi_error_code {
	__s32 err;
	__u32 padding;
};

struct kvmi_get_version_reply {
	__s32 err;
	__u32 version;
};

struct kvmi_get_guest_info_reply {
	__s32 err;
	__u16 vcpu_count;
	__u16 padding;
	__u64 tsc_speed;
};

struct kvmi_get_registers_x86 {
	__u16 vcpu;
	__u16 nmsrs;
	__u32 msrs_idx[0];
};

struct kvmi_get_registers_x86_reply {
	__s32 err;
	__u32 mode;
	struct kvm_regs regs;
	struct kvm_sregs sregs;
	struct kvm_msrs msrs;
};

struct kvmi_set_registers_x86 {
	__u16 vcpu;
	__u16 padding[3];
	struct kvm_regs regs;
};

struct kvmi_mtrr_type {
	__u64 gpa;
};

struct kvmi_mtrr_type_reply {
	__s32 err;
	__u32 padding;
	__u64 type;
};

struct kvmi_mtrrs {
	__u16 vcpu;
	__u16 padding[3];
};

struct kvmi_mtrrs_reply {
	__s32 err;
	__u32 padding;
	__u64 pat;
	__u64 cap;
	__u64 type;
};

struct kvmi_xsave_info {
	__u16 vcpu;
	__u16 padding[3];
};

struct kvmi_xsave_info_reply {
	__s32 err;
	__u32 size;
};

struct kvmi_get_page_access {
	__u16 vcpu;
	__u16 padding[3];
	__u64 gpa;
};

struct kvmi_get_page_access_reply {
	__s32 err;
	__u32 access;
};

struct kvmi_set_page_access {
	__u16 vcpu;
	__u16 padding;
	__u32 access;
	__u64 gpa;
};

struct kvmi_page_fault {
	__u16 vcpu;
	__u16 padding;
	__u32 error;
	__u64 gva;
};

struct kvmi_inject_breakpoint {
	__u16 vcpu;
	__u16 padding[3];
};

struct kvmi_map_physical_page_to_guest {
	__u64 gpa_src;
	__u64 gfn_dest;
};

struct kvmi_unmap_physical_page_from_guest {
	__u64 gfn_dest;
};

struct kvmi_control_events {
	__u16 vcpu;
	__u16 padding;
	__u32 events;
};

struct kvmi_cr_control {
	__u8 enable;
	__u8 padding[3];
	__u32 cr;
};

struct kvmi_msr_control {
	__u8 enable;
	__u8 padding[3];
	__u32 msr;
};

struct kvmi_event_x86 {
	__u16 vcpu;
	__u8 mode;
	__u8 padding1;
	__u32 event;
	struct kvm_regs regs;
	struct kvm_sregs sregs;
	struct {
		__u64 sysenter_cs;
		__u64 sysenter_esp;
		__u64 sysenter_eip;
		__u64 efer;
		__u64 star;
		__u64 lstar;
	} msrs;
};

struct kvmi_event_x86_reply {
	struct kvm_regs regs;
	__u32 actions;
	__u32 padding;
};

struct kvmi_event_cr {
	__u16 cr;
	__u16 padding[3];
	__u64 old_value;
	__u64 new_value;
};

struct kvmi_event_cr_reply {
	__u64 new_val;
};

struct kvmi_event_msr {
	__u32 msr;
	__u32 padding;
	__u64 old_value;
	__u64 new_value;
};

struct kvmi_event_msr_reply {
	__u64 new_val;
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

struct kvmi_event_page_fault_reply {
	__u32 ctx_size;
	__u8 ctx_data[256];
};

struct kvmi_event_trap {
	__u32 vector;
	__u32 type;
	__u32 err;
	__u32 padding;
	__u64 cr2;
};

#endif /* __KVMI_H_INCLUDED__ */
