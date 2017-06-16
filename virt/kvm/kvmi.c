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
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/list.h>
#include <linux/uuid.h>
#include <linux/poll.h>
#include <linux/vmalloc.h>
#include <linux/anon_inodes.h>
#include <linux/uaccess.h>
#include <asm/pgtable_types.h>
#include <linux/mmu_context.h>
#include <uapi/linux/kvmi.h>
#include <linux/uuid.h>
#include <linux/hashtable.h>
#include <linux/kconfig.h>
#include "../../arch/x86/kvm/x86.h"
#include "../../arch/x86/kvm/mmu.h"
#include <net/sock.h>
#include <net/af_vsock.h>
#include "kvmi_socket.h"

struct kvmi_mem_access {
	struct list_head link;
	gfn_t gfn;
	unsigned int access;
};

struct kvm_enum_param {
	unsigned int k;
	unsigned int n;
	struct kvmi_guests *guests;
};

struct resp_info {
	size_t to_read;
	int vcpu_req;
	int (*cb)(void *s, struct kvm *, struct kvmi_socket_hdr *req,
		  void *i);
};

struct ev_recv {
	struct hlist_node list;
	struct completion ready;
	struct kvmi_socket_hdr h;
	void *buf;
	size_t buf_size;
	bool processing;
	bool received;
};

static bool accept_socket_cb(void *ctx, kvmi_socket_read_cb read_cb,
			     void *cb_ctx);
static bool consume_bytes_from_socket(size_t n, kvmi_socket_read_cb read_cb,
				      void *ctx);
static bool guest_recv_cb(void *ctx, kvmi_socket_read_cb read_cb, void *cb_ctx);
static bool main_recv_cb(void *ctx, kvmi_socket_read_cb read_cb, void *cb_ctx);
static bool send_vcpu_event_and_wait(struct kvm_vcpu *vcpu, void *ev,
				     size_t ev_size, void *resp,
				     size_t resp_size);
static const char *id2str(int i);
static int cnt_cb(const struct kvm *kvm, void *p);
static int connect_handler_if_missing(void *s, struct kvm *kvm,
				      kvmi_socket_use_cb recv_cb);
static int copy_guest_cb(const struct kvm *kvm, void *param);
static int get_msr_cb(struct kvm_vcpu *vcpu, void *ctx);
static int get_mttr_memory_type_cb(struct kvm_vcpu *vcpu, void *ctx);
static int get_page_info_cb(struct kvm_vcpu *vcpu, void *ctx);
static int get_registers_cb(struct kvm_vcpu *vcpu, void *ctx);
static int get_tsc_cb(struct kvm_vcpu *vcpu, void *ctx);
static int get_vcpu(struct kvm *kvm, int vcpu_id, struct kvm_vcpu **vcpu);
static int get_xstate_size_cb(struct kvm_vcpu *vcpu, void *ctx);
static int inject_breakpoint_cb(struct kvm_vcpu *vcpu, void *ctx);
static int inject_pf_cb(struct kvm_vcpu *vcpu, void *ctx);
static int query_locked_vcpu(struct kvm *kvm, int vcpu_id,
			     int (*cb)(struct kvm_vcpu *, void *), void *ctx);
static int query_paused_vcpu(struct kvm *kvm, int vcpu_id,
			     int (*cb)(struct kvm_vcpu *, void *), void *ctx);
static int query_paused_vm(struct kvm *kvm, int (*cb) (struct kvm *, void *),
			   void *ctx);
static int respond_cr_control(void *s, struct kvm *kvm,
			      struct kvmi_socket_hdr *req, void *_i);
static int respond_event_control(void *s, struct kvm *kvm,
				 struct kvmi_socket_hdr *req, void *_i);
static int respond_get_guest_info(void *s, struct kvm *kvm,
				  struct kvmi_socket_hdr *req, void *i);
static int respond_get_guests(void *s, struct kvmi_socket_hdr *req);
static int respond_get_mtrr_type(void *s, struct kvm *kvm,
				 struct kvmi_socket_hdr *req, void *i);
static int respond_get_mtrrs(void *s, struct kvm *kvm,
			     struct kvmi_socket_hdr *req, void *i);
static int respond_get_page_access(void *s, struct kvm *kvm,
				   struct kvmi_socket_hdr *req, void *_i);
static int respond_get_registers(void *s, struct kvm *kvm,
				 struct kvmi_socket_hdr *req, void *i);
static int respond_get_version(void *s, struct kvm *kvm,
			       struct kvmi_socket_hdr *req, void *i);
static int respond_get_xsave_info(void *s, struct kvm *kvm,
				  struct kvmi_socket_hdr *req, void *i);
static int respond_inject_breakpoint(void *s, struct kvm *kvm,
				     struct kvmi_socket_hdr *req, void *_i);
static int respond_inject_page_fault(void *s, struct kvm *kvm,
				     struct kvmi_socket_hdr *req, void *_i);
static int respond_map_physical_page_to_sva(void *s, struct kvm *kvm,
					    struct kvmi_socket_hdr *req,
					    void *_i);
static int respond_unmap_physical_page_from_sva(void *s, struct kvm *kvm,
						struct kvmi_socket_hdr *req,
						void *_i);
static int respond_msr_control(void *s, struct kvm *kvm,
			       struct kvmi_socket_hdr *req, void *_i);
static int respond_pause_guest(void *s, struct kvm *kvm,
			       struct kvmi_socket_hdr *req, void *i);
static int respond_read_physical(void *s, struct kvm *kvm,
				 struct kvmi_socket_hdr *req, void *_i);
static int respond_set_page_access(void *s, struct kvm *kvm,
				   struct kvmi_socket_hdr *req, void *_i);
static int respond_set_registers(void *s, struct kvm *kvm,
				 struct kvmi_socket_hdr *req, void *i);
static int respond_shutdown_guest(void *s, struct kvm *kvm,
				  struct kvmi_socket_hdr *req, void *i);
static int respond_to_request(void *s, struct kvmi_socket_hdr *req, void *buf,
			      size_t size);
static int respond_to_request_buf(void *s, struct kvmi_socket_hdr *req,
				  const void *buf, size_t size);
static int respond_unpause_guest(void *s, struct kvm *kvm,
				 struct kvmi_socket_hdr *req, void *i);
static int respond_with_error_code(void *s, int err, struct kvmi_socket_hdr *h);
static int respond_write_physical(void *s, struct kvm *kvm,
				  struct kvmi_socket_hdr *req, void *_i);
static int send_async_event_to_socket(struct kvm *kvm, struct kvec *i, size_t n,
				      size_t bytes);
static int set_cr_control(struct kvm *kvm, void *ctx);
static int set_msr_control(struct kvm *kvm, void *ctx);
static int set_page_info_cb(struct kvm_vcpu *vcpu, void *ctx);
static int set_registers_cb(struct kvm_vcpu *vcpu, void *ctx);
static u32 new_seq(void);
static void __release_kvm_socket(struct kvm *kvm);
static void send_event(struct kvm *kvm, int msg_id, void *data, size_t size);
static void wakeup_events(struct kvm *kvm);

static struct kvm dummy;
static struct kvm *sva;
static atomic_t seq_ev = ATOMIC_INIT(0);
static struct resp_info guest_responses[] = {
	{0, 0, NULL},
	{0, 0, respond_get_version},
	{0, 0, NULL},		/* KVMI_GET_GUESTS */
	{0, 2, respond_get_guest_info},
	{0, 0, respond_pause_guest},
	{0, 0, respond_unpause_guest},
	{-1, 1, respond_get_registers},
	{sizeof(struct kvmi_set_registers), 1, respond_set_registers},
	{0, 0, respond_shutdown_guest},
	{sizeof(__u64), 2, respond_get_mtrr_type},
	{sizeof(__u16), 1, respond_get_mtrrs},
	{sizeof(__u16), 1, respond_get_xsave_info},
	{sizeof(struct kvmi_page_access), 1, respond_get_page_access},
	{sizeof(struct kvmi_page_access), 1, respond_set_page_access},
	{sizeof(struct kvmi_page_fault), 1, respond_inject_page_fault},
	{sizeof(struct kvmi_rw_physical_info), 0, respond_read_physical},
	{-1, 0, respond_write_physical},	/* TODO: avoid kalloc+memcpy */
	{sizeof(struct kvmi_map_physical_to_sva_info), 0,
	 respond_map_physical_page_to_sva},
	{sizeof(struct kvmi_unmap_physical_from_sva_info), 0,
	 respond_unmap_physical_page_from_sva},
	{sizeof(struct kvmi_event_control), 1, respond_event_control},
	{sizeof(struct kvmi_cr_control), 0, respond_cr_control},
	{sizeof(struct kvmi_msr_control), 0, respond_msr_control},
	{sizeof(__u16), 1, respond_inject_breakpoint},
};

static char *IDs[] = {
	"KVMI_NULL???",
	"KVMI_GET_VERSION",
	"KVMI_GET_GUESTS",
	"KVMI_GET_GUEST_INFO",
	"KVMI_PAUSE_GUEST",
	"KVMI_UNPAUSE_GUEST",
	"KVMI_GET_REGISTERS",
	"KVMI_SET_REGISTERS",
	"KVMI_SHUTDOWN_GUEST",
	"KVMI_GET_MTRR_TYPE",
	"KVMI_GET_MTRRS",
	"KVMI_GET_XSAVE_INFO",
	"KVMI_GET_PAGE_ACCESS",
	"KVMI_SET_PAGE_ACCESS",
	"KVMI_INJECT_PAGE_FAULT",
	"KVMI_READ_PHYSICAL",
	"KVMI_WRITE_PHYSICAL",
	"KVMI_MAP_PHYSICAL_PAGE_TO_SVA",
	"KVMI_UNMAP_PHYSICAL_PAGE_TO_SVA",
	"KVMI_EVENT_CONTROL",
	"KVMI_CR_CONTROL",
	"KVMI_MSR_CONTROL",
	"KVMI_INJECT_BREAKPOINT",
	"KVMI_EVENT_GUEST_ON",
	"KVMI_EVENT_GUEST_OFF",
	"KVMI_EVENT_VCPU",
	"KVMI_REPLY_EVENT_VCPU",
};

#define REQ_PAUSE  0
#define REQ_RESUME 1
#define REQ_CMD    2
#define REQ_REPLY  3
#define REQ_CLOSE  4

static void set_sem_req(int req, struct kvm_vcpu *vcpu)
{
	set_bit(req, &vcpu->sem_requests);
	/* Make sure the bit is set when the worker wakes up */
	smp_wmb();
	up(&vcpu->sock_sem);
}

static void clear_sem_req(int req, struct kvm_vcpu *vcpu)
{
	clear_bit(req, &vcpu->sem_requests);
}

static int vm_pause(struct kvm *kvm)
{
	int i;
	struct kvm_vcpu *vcpu;

	mutex_lock(&kvm->lock);
	kvm_for_each_vcpu(i, vcpu, kvm) {
		size_t cnt = READ_ONCE(vcpu->pause_count);

		WRITE_ONCE(vcpu->pause_count, cnt + 1);
		if (!cnt) {
			set_sem_req(REQ_PAUSE, vcpu);
			kvm_make_request(KVM_REQ_INTROSPECTION, vcpu);
			kvm_vcpu_kick(vcpu);
			while (test_bit(REQ_PAUSE, &vcpu->sem_requests))
				;
		}
	}
	mutex_unlock(&kvm->lock);
	return 0;
}

static int vm_resume(struct kvm *kvm)
{
	int i;
	struct kvm_vcpu *vcpu;

	mutex_lock(&kvm->lock);
	kvm_for_each_vcpu(i, vcpu, kvm) {
		size_t cnt = READ_ONCE(vcpu->pause_count);

		WARN_ON(cnt == 0);
		WRITE_ONCE(vcpu->pause_count, cnt - 1);
		if (cnt == 1) {
			set_sem_req(REQ_RESUME, vcpu);
			while (test_bit(REQ_RESUME, &vcpu->sem_requests))
				;
		}
	}
	mutex_unlock(&kvm->lock);
	return 0;
}

static int kvmi_set_mem_access(struct kvm *kvm, unsigned long gpa,
			       unsigned int access)
{
	struct kvmi_mem_access *m;
	struct kvmi_mem_access *__m;

	m = kzalloc(sizeof(struct kvmi_mem_access), GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	INIT_LIST_HEAD(&m->link);
	m->gfn = gpa_to_gfn(gpa);
	m->access = access;

	mutex_lock(&kvm->access_tree_lock);
	__m = radix_tree_lookup(&kvm->access_tree, m->gfn);
	if (__m) {
		__m->access = m->access;
		if (list_empty(&__m->link))
			list_add_tail(&__m->link, &kvm->access_list);
	} else {
		radix_tree_insert(&kvm->access_tree, m->gfn, m);
		list_add_tail(&m->link, &kvm->access_list);
		m = NULL;
	}
	mutex_unlock(&kvm->access_tree_lock);

	kfree(m);

	return 0;
}

static bool kvmi_test_mem_access(struct kvm *kvm, unsigned long gpa,
				 unsigned int exception_flags)
{
	struct kvmi_mem_access *m;
	bool report = false;

	mutex_lock(&kvm->access_tree_lock);
	m = radix_tree_lookup(&kvm->access_tree, gpa_to_gfn(gpa));
	mutex_unlock(&kvm->access_tree_lock);

	if (m) {
		bool missing_ept_paging_structs =
		    (((exception_flags >> 3) & 7) == 0);
		report = !missing_ept_paging_structs;
	}

	return report;
}

static void kvmi_apply_mem_access(struct kvm_vcpu *vcpu, gfn_t gfn,
				  unsigned int access)
{
	int err;
	gpa_t gpa = gfn << PAGE_SHIFT;
	struct kvm *kvm = vcpu->kvm;

	err = kvm_mmu_set_spte(kvm, vcpu, gpa,
			       access & 1, access & 2, access & 4);
	if (err < 0) {
		u32 error_code = PFERR_PRESENT_MASK;

		/* The entry is not present. Tell the MMU to create it */
		err = vcpu->arch.mmu.page_fault(vcpu, gpa, error_code, false);

		if (!err) {
			err = kvm_mmu_set_spte(kvm, vcpu, gpa,
					       access & 1,
					       access & 2, access & 4);
		}

		if (err < 0)
			kvm_err("%s: page_fault: %d (gpa:%llX)\n", __func__,
				err, gpa);
	}

	if (err > 0)
		kvm_make_request(KVM_REQ_TLB_FLUSH, vcpu);
}

void kvmi_flush_mem_access(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;

	mutex_lock(&kvm->access_tree_lock);
	while (!list_empty(&kvm->access_list)) {
		struct kvmi_mem_access *m =
		    list_first_entry(&kvm->access_list, struct kvmi_mem_access,
				     link);

		list_del(&m->link);
		INIT_LIST_HEAD(&m->link);

		kvmi_apply_mem_access(vcpu, m->gfn, m->access);
	}
	mutex_unlock(&kvm->access_tree_lock);
}

static void kvmi_free_mem_access(struct kvm *kvm)
{
	void **slot;
	struct radix_tree_iter iter;

	radix_tree_for_each_slot(slot, &kvm->access_tree, &iter, 0) {
		struct kvmi_mem_access *m = *slot;

		radix_tree_delete(&kvm->access_tree, m->gfn);
		kfree(m);
	}
}

static unsigned long *msr_mask(struct kvm *kvm, unsigned int *msr)
{
	switch (*msr) {
	case 0 ... 0x1fff:
		return kvm->msr_mask.low;
	case 0x40000000 ... 0x40001fff:
		*msr &= 0x1fff;
		return kvm->msr_mask.hypervisor;
	case 0xc0000000 ... 0xc0001fff:
		*msr &= 0x1fff;
		return kvm->msr_mask.high;
	}
	return NULL;
}

static int msr_control(struct kvm *kvm, unsigned int msr, bool enable)
{
	unsigned long *mask = msr_mask(kvm, &msr);

	if (!mask)
		return -EINVAL;
	if (enable)
		set_bit(msr, mask);
	else
		clear_bit(msr, mask);
	return 0;
}

static void kvmi_cleanup(struct kvm *kvm)
{
	write_lock(&kvm->socket_ctx_lock);
	__release_kvm_socket(kvm);
	write_unlock(&kvm->socket_ctx_lock);

	kvmi_free_mem_access(kvm);
	kvm->introduced = 0;
	/* TODO */
	smp_wmb();
}

static unsigned int kvmi_vcpu_mode(const struct kvm_vcpu *vcpu,
				   const struct kvm_sregs *sregs)
{
	unsigned int mode = 0;

	if (is_long_mode((struct kvm_vcpu *) vcpu)) {
		if (sregs->cs.l)
			mode = 8;
		else if (!sregs->cs.db)
			mode = 2;
		else
			mode = 4;
	} else if (sregs->cr0 & X86_CR0_PE) {
		if (!sregs->cs.db)
			mode = 2;
		else
			mode = 4;
	} else if (!sregs->cs.db)
		mode = 2;
	else
		mode = 4;

	return mode;
}

int kvmi_init(void)
{
	rwlock_init(&dummy.socket_ctx_lock);
	dummy.introduced = 1;

	/* TODO: change ANY to a specific CID */
	return kvmi_socket_start_vsock(VMADDR_CID_ANY, 1234, accept_socket_cb,
				       &dummy);
}

void kvmi_uninit(void)
{
	dummy.introduced = 0;

	__release_kvm_socket(&dummy);
	kvmi_socket_stop();
}

void kvmi_vm_powered_on(struct kvm *kvm)
{
	if (sva)
		send_event(&dummy, KVMI_EVENT_GUEST_ON, &kvm->uuid,
			   sizeof(kvm->uuid));
}

void kvmi_vm_powered_off(struct kvm *kvm)
{
	if (sva && kvm != sva)
		send_event(&dummy, KVMI_EVENT_GUEST_OFF, &kvm->uuid,
			   sizeof(kvm->uuid));
	kvmi_cleanup(kvm);
}

static void kvm_get_msrs(struct kvm_vcpu *vcpu, struct kvmi_event *event)
{
	struct msr_data msr;

	msr.host_initiated = true;

	msr.index = MSR_IA32_SYSENTER_CS;
	kvm_get_msr(vcpu, &msr);
	event->msrs.sysenter_cs = msr.data;

	msr.index = MSR_IA32_SYSENTER_ESP;
	kvm_get_msr(vcpu, &msr);
	event->msrs.sysenter_esp = msr.data;

	msr.index = MSR_IA32_SYSENTER_EIP;
	kvm_get_msr(vcpu, &msr);
	event->msrs.sysenter_eip = msr.data;

	msr.index = MSR_EFER;
	kvm_get_msr(vcpu, &msr);
	event->msrs.efer = msr.data;

	msr.index = MSR_STAR;
	kvm_get_msr(vcpu, &msr);
	event->msrs.star = msr.data;

	msr.index = MSR_LSTAR;
	kvm_get_msr(vcpu, &msr);
	event->msrs.lstar = msr.data;
}

static void kvmi_load_regs(struct kvm_vcpu *vcpu, struct kvmi_event *event)
{
	kvm_arch_vcpu_ioctl_get_regs(vcpu, &event->regs);
	kvm_arch_vcpu_ioctl_get_sregs(vcpu, &event->sregs);
	kvm_get_msrs(vcpu, event);

	event->mode = kvmi_vcpu_mode(vcpu, &event->sregs);
}

bool kvmi_cr_event(struct kvm_vcpu *vcpu, unsigned int cr,
		   unsigned long old_value, unsigned long *new_value)
{
	struct kvm *kvm = vcpu->kvm;
	unsigned long event_mask = atomic_read(&kvm->event_mask);
	struct kvmi_event vm_event = {
		.vcpu = vcpu->vcpu_id,
		.event = KVMI_EVENT_CR,
		.cr.cr = cr,
		.cr.old_value = old_value,
		.cr.new_value = *new_value
	};
	struct kvmi_event_reply r;

	/* Is anyone interested in this event? */
	if (!(KVMI_EVENT_CR & event_mask))
		return true;
	if (!test_bit(cr, &kvm->cr_mask))
		return true;
	if (old_value == *new_value)
		return true;

	kvmi_load_regs(vcpu, &vm_event);

	if (!send_vcpu_event_and_wait
	    (vcpu, &vm_event, sizeof(vm_event), &r, sizeof(r)))
		return true;

	if (r.event & KVMI_EVENT_SET_REGS)
		kvm_arch_vcpu_set_regs(vcpu, &r.regs);

	if (r.event & KVMI_EVENT_ALLOW) {
		*new_value = r.new_val;
		return true;
	}

	return false;
}

bool kvmi_msr_event(struct kvm_vcpu *vcpu, unsigned int msr, u64 old_value,
		    u64 *new_value)
{
	unsigned long event_mask;
	unsigned long *mask;
	struct kvm *kvm = vcpu->kvm;
	struct kvmi_event vm_event = {
		.vcpu = vcpu->vcpu_id,
		.event = KVMI_EVENT_MSR,
		.msr.msr = msr,
		.msr.old_value = old_value,
		.msr.new_value = *new_value
	};
	struct kvmi_event_reply r;

	/* Is anyone interested in this event? */
	event_mask = atomic_read(&kvm->event_mask);
	if (!(KVMI_EVENT_MSR & event_mask))
		return true;
	mask = msr_mask(kvm, &msr);
	if (!mask)
		return true;
	if (!test_bit(msr, mask))
		return true;

	kvmi_load_regs(vcpu, &vm_event);

	if (!send_vcpu_event_and_wait
	    (vcpu, &vm_event, sizeof(vm_event), &r, sizeof(r)))
		return true;

	if (r.event & KVMI_EVENT_SET_REGS)
		kvm_arch_vcpu_set_regs(vcpu, &r.regs);

	if (r.event & KVMI_EVENT_ALLOW) {
		*new_value = r.new_val;
		return true;
	}

	return false;
}

void kvmi_xsetbv_event(struct kvm_vcpu *vcpu, u64 value)
{
	struct kvm *kvm = vcpu->kvm;
	unsigned long event_mask = atomic_read(&kvm->event_mask);
	struct kvmi_event vm_event = {
		.vcpu = vcpu->vcpu_id,
		.event = KVMI_EVENT_XSETBV,
		.xsetbv.xcr0 = value
	};
	struct kvmi_event_reply r;

	/* Is anyone interested in this event? */
	if (!(KVMI_EVENT_XSETBV & event_mask))
		return;

	kvmi_load_regs(vcpu, &vm_event);

	if (!send_vcpu_event_and_wait
	    (vcpu, &vm_event, sizeof(vm_event), &r, sizeof(r)))
		return;

	if (r.event & KVMI_EVENT_SET_REGS)
		kvm_arch_vcpu_set_regs(vcpu, &r.regs);
}

bool kvmi_breakpoint_event(struct kvm_vcpu *vcpu, u64 gpa)
{
	struct kvm *kvm = vcpu->kvm;
	unsigned long event_mask = atomic_read(&kvm->event_mask);
	struct kvmi_event vm_event = {
		.vcpu = vcpu->vcpu_id,
		.event = KVMI_EVENT_BREAKPOINT,
		.breakpoint.gpa = gpa
	};
	struct kvmi_event_reply r;

	/* Is anyone interested in this event? */
	if (!(KVMI_EVENT_BREAKPOINT & event_mask))
		return true;

	kvmi_load_regs(vcpu, &vm_event);

	if (!send_vcpu_event_and_wait
	    (vcpu, &vm_event, sizeof(vm_event), &r, sizeof(r)))
		return true;

	if (r.event & KVMI_EVENT_SET_REGS)
		kvm_arch_vcpu_set_regs(vcpu, &r.regs);

	if (r.event & KVMI_EVENT_ALLOW)
		return true;

	return false;
}

void kvmi_vmcall_event(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	unsigned long event_mask = atomic_read(&kvm->event_mask);
	struct kvmi_event vm_event = {
		.vcpu = vcpu->vcpu_id,
		.event = KVMI_EVENT_USER_CALL
	};
	struct kvmi_event_reply r;

	/* Is anyone interested in this event? */
	if (!(KVMI_EVENT_USER_CALL & event_mask))
		return;

	kvmi_load_regs(vcpu, &vm_event);

	if (!send_vcpu_event_and_wait
	    (vcpu, &vm_event, sizeof(vm_event), &r, sizeof(r)))
		return;

	if (r.event & KVMI_EVENT_SET_REGS)
		kvm_arch_vcpu_set_regs(vcpu, &r.regs);
}

bool kvmi_page_fault(struct kvm_vcpu *vcpu, unsigned long gpa,
		     unsigned long gva, unsigned int mode, unsigned int *opts)
{
	struct kvm *kvm = vcpu->kvm;
	unsigned long event_mask = atomic_read(&kvm->event_mask);
	struct kvmi_event vm_event = {
		.vcpu = vcpu->vcpu_id,
		.event = KVMI_EVENT_PAGE_FAULT,
		.page_fault.gpa = gpa,
		.page_fault.gva = gva,
		.page_fault.mode = mode
	};
	struct kvmi_event_reply r;
	bool emulate = false;

	/* Is anyone interested in this event? */
	if (!(KVMI_EVENT_PAGE_FAULT & event_mask))
		return emulate;

	/* Have we shown interest in this page? */
	if (!kvmi_test_mem_access(kvm, gpa, mode))
		return emulate;

	kvmi_load_regs(vcpu, &vm_event);

	if (!send_vcpu_event_and_wait
	    (vcpu, &vm_event, sizeof(vm_event), &r, sizeof(r)))
		return emulate;

	emulate = (r.event & KVMI_EVENT_ALLOW);

	if (r.event & KVMI_EVENT_SET_REGS)
		kvm_arch_vcpu_set_regs(vcpu, &r.regs);

	*opts = r.event & (KVMI_EVENT_NOEMU | KVMI_EVENT_SET_CTX);

	if (r.event & KVMI_EVENT_SET_CTX) {
		u32 size = min(sizeof(vcpu->ctx_data), sizeof(r.ctx_data));

		memcpy(vcpu->ctx_data, r.ctx_data, size);
		vcpu->ctx_size = size;
		vcpu->ctx_pos = 0;
	} else {
		vcpu->ctx_size = 0;
		vcpu->ctx_pos = 0;
	}

	return emulate;
}

void kvmi_trap_event(struct kvm_vcpu *vcpu, unsigned int vector,
		     unsigned int type, unsigned int err, u64 cr2)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvmi_event vm_event = {
		.vcpu = vcpu->vcpu_id,
		.event = KVMI_EVENT_TRAP,
		.trap.vector = vector,
		.trap.type = type,
		.trap.err = err,
		.trap.cr2 = cr2
	};
	struct kvmi_event_reply r;

	unsigned long event_mask = atomic_read(&kvm->event_mask);

	if (!(KVMI_EVENT_TRAP & event_mask))
		return;

	if (!atomic_read(&vcpu->arch.next_interrupt_enabled))
		return;
	atomic_set(&vcpu->arch.next_interrupt_enabled, 0);

	kvmi_load_regs(vcpu, &vm_event);

	if (!send_vcpu_event_and_wait
	    (vcpu, &vm_event, sizeof(vm_event), &r, sizeof(r)))
		return;

	if (r.event & KVMI_EVENT_SET_REGS)
		kvm_arch_vcpu_set_regs(vcpu, &r.regs);
}

bool accept_socket_cb(void *ctx, kvmi_socket_read_cb read_cb, void *cb_ctx)
{
	int is_main;
	uuid_le id;
	struct kvm *kvm = ctx;	/* &dummy */
	int err;
	bool closing = (read_cb == NULL);

	if (closing) {
		kvm_info("%s: closing\n", __func__);
		return false;
	}

	/* TODO: validate sva */
	err = read_cb(cb_ctx, &id, sizeof(id));

	if (err) {
		kvm_err("%s: read: %d\n", __func__, err);
		return false;
	}

	is_main = (uuid_le_cmp(id, NULL_UUID_LE) == 0);

	/* TODO: use kvm_get with every new onnection */

	if (is_main) {
		err = connect_handler_if_missing(cb_ctx, kvm, main_recv_cb);
	} else if (sva && uuid_le_cmp(id, sva->uuid) == 0) {
		kvm_info("Avoid self-introspection\n");
		err = -EPERM;
	} else {
		struct kvm *g = kvm_from_uuid(&id);

		if (g) {
			err = connect_handler_if_missing(cb_ctx, g,
							 guest_recv_cb);
			kvm_put_kvm(g);
		} else {
			err = -ENOENT;
		}
	}

	if (err)
		kvm_err("%s: connect %s: %d\n", __func__,
			is_main ? "main" : "guest", err);

	return (err == 0);
}

int connect_handler_if_missing(void *s, struct kvm *kvm,
			       kvmi_socket_use_cb recv_cb)
{
	void *ctx;
	int err = 0;

	write_lock(&kvm->socket_ctx_lock);

	if (kvm->socket_ctx && kvmi_socket_is_active(kvm->socket_ctx)) {
		err = -EEXIST;
		goto unlock;
	}

	/*
	 * We can lose a new connection if the old one didn't finished closing,
	 * but we expect another connection attempt.
	 */

	__release_kvm_socket(kvm);
	ctx = kvmi_socket_monitor(s, recv_cb, kvm);

	if (IS_ERR(ctx)) {
		err = (int) PTR_ERR(ctx);
		goto unlock;
	}

	kvm->socket_ctx = ctx;
unlock:
	write_unlock(&kvm->socket_ctx_lock);
	return err;
}

/*
 * The other side must use one send/write call
 * in order to avoid the need for reconstruction in this function.
 */
bool main_recv_cb(void *ctx, kvmi_socket_read_cb read_cb, void *cb_ctx)
{
	struct kvmi_socket_hdr h;
	int err;
	bool closing = (read_cb == NULL);
	static bool first = true;

	if (closing) {
		kvm_info("%s: closing\n", __func__);
		first = true;
		if (sva) {
			kvm_put_kvm(sva);
			sva = NULL;
		}
		return false;
	}

	if (first) {		/* TODO: pack it into a KVMI_ message */
		uuid_le sva_id;

		err = read_cb(cb_ctx, &sva_id, sizeof(sva_id));
		if (err) {
			kvm_err("%s: error getting sva err:%d\n", __func__,
				err);
			return false;
		}
		sva = kvm_from_uuid(&sva_id);	/* TODO: lock ? */
		if (!sva) {
			kvm_err("%s: can't find sva\n", __func__);
			return false;
		}
		first = false;
	}

	err = read_cb(cb_ctx, &h, sizeof(h));

	if (err) {
		kvm_err("%s/%p: id:%d (%s) size:%u seq:%u err:%d\n", __func__,
			cb_ctx, h.msg_id, id2str(h.msg_id), h.size, h.seq, err);
		return false;
	}

	kvm_debug("%s: id:%d (%s) size:%u\n", __func__, h.msg_id,
		  id2str(h.msg_id), h.size);

	switch (h.msg_id) {
	case KVMI_GET_VERSION:
		err = respond_get_version(cb_ctx, &dummy, &h, NULL);
		break;

	case KVMI_GET_GUESTS:
		err = respond_get_guests(cb_ctx, &h);
		break;

	default:
		kvm_err("%s: unknown message 0x%x of %u bytes\n", __func__,
			h.msg_id, h.size);
		return consume_bytes_from_socket(h.size, read_cb, cb_ctx);
	}

	if (err) {
		kvm_err("%s: id:%d (%s) err:%d\n", __func__, h.msg_id,
			id2str(h.msg_id), err);
		return false;
	}

	return true;
}

const char *id2str(int i)
{
	return (i > 0 && i < ARRAY_SIZE(IDs) ? IDs[i] : "unknown");
}

static bool handle_event_reply(struct kvm *kvm, struct kvmi_socket_hdr *h,
			       kvmi_socket_read_cb read_cb, void *cb_ctx)
{
	int i;
	struct kvm_vcpu *vcpu;
	bool found_seq = false;
	bool ok = false;

	mutex_lock(&kvm->lock);
	kvm_for_each_vcpu(i, vcpu, kvm) {
		if (READ_ONCE(vcpu->sock_rsp_waiting)
		    && h->seq == vcpu->sock_rsp_seq) {
			found_seq = true;
			break;
		}
	}
	mutex_unlock(&kvm->lock);

	if (!found_seq) {
		kvm_err("%s: unexpected event reply (seq=%u)\n", __func__,
			h->seq);
		return false;
	}

	if (h->size > vcpu->sock_rsp_size) {
		kvm_err("%s: event reply too big (max=%lu, recv=%u)\n",
		     __func__, vcpu->sock_rsp_size, h->size);
	} else {
		int err = read_cb(cb_ctx, vcpu->sock_rsp_buf, h->size);

		if (!err)
			ok = true;
		else
			kvm_err("%s: reply err: %d\n", __func__, err);
	}

	WARN_ON(h->size == 0);

	WRITE_ONCE(vcpu->sock_rsp_received, ok ? h->size : -1);

	set_sem_req(REQ_REPLY, vcpu);

	return ok;
}

/*
 * The other side must use one send/write call
 * in order to avoid the need for reconstruction in this function.
 */
bool guest_recv_cb(void *ctx, kvmi_socket_read_cb read_cb, void *cb_ctx)
{
	struct kvm *kvm = ctx;
	struct kvmi_socket_hdr h;
	struct resp_info *r;
	u8 tmp[256];
	void *i = (void *) tmp;
	int err;
	bool closing = (read_cb == NULL);

	if (closing) {
		kvm_info("%s: closing\n", __func__);

		/* We are no longer interested in any kind of events */
		atomic_set(&kvm->event_mask, 0);
		kvm->cr_mask = 0;
		memset(&kvm->msr_mask, 0, sizeof(kvm->msr_mask));
		/* TODO */
		smp_wmb();

		wakeup_events(kvm);

		return false;
	}

	err = read_cb(cb_ctx, &h, sizeof(h));

	if (err) {
		kvm_err("%s/%p: id:%d (%s) size:%u seq:%u err:%d\n", __func__,
			cb_ctx, h.msg_id, id2str(h.msg_id), h.size, h.seq, err);
		return false;
	}

	kvm_debug("%s: id:%d (%s) size:%u\n", __func__, h.msg_id,
		  id2str(h.msg_id), h.size);

	if (h.msg_id == KVMI_REPLY_EVENT_VCPU)
		return handle_event_reply(kvm, &h, read_cb, cb_ctx);

	r = guest_responses + h.msg_id;

	if (h.msg_id >= ARRAY_SIZE(guest_responses) || !r->cb) {
		kvm_err("%s: unknown message 0x%x of %u bytes\n", __func__,
			h.msg_id, h.size);
		return consume_bytes_from_socket(h.size, read_cb, cb_ctx);
	}

	if (r->to_read != h.size && r->to_read != (size_t) -1) {
		kvm_err("%s: %u instead of %u bytes\n", __func__, h.size,
			(unsigned int) r->to_read);
		return false;
	}

	if (r->to_read) {
		size_t chunk = r->to_read;

		if (chunk == (size_t) -1)
			chunk = h.size;

		if (chunk > sizeof(tmp))
			i = kmalloc(chunk, GFP_KERNEL);

		if (!i)
			return false;

		err = read_cb(cb_ctx, i, chunk);
		if (err)
			goto out;
	}

	if (r->vcpu_req == 0) {
		err = r->cb(cb_ctx, kvm, &h, i);
	} else {
		u16 vcpu_id;
		struct kvm_vcpu *vcpu;

		if (r->vcpu_req > 1) {
			vcpu_id = 0;
		} else {
			if (h.size < sizeof(vcpu_id)) {
				kvm_err("%s: invalid message\n", __func__);
				err = -E2BIG;
				goto out;
			}
			vcpu_id = *((u16 *) i);
		}
		err = get_vcpu(kvm, vcpu_id, &vcpu);
		if (err) {
			kvm_err("%s: invalid vcpu:%d err:%d\n", __func__,
				vcpu_id, err);
			goto out;
		}
		if (test_bit(REQ_CMD, &vcpu->sem_requests)) {
			kvm_err("%s: vcpu %d is busy\n", __func__, vcpu_id);
			err = -EBUSY;
			goto out;
		}
		if (h.size > sizeof(vcpu->sock_cmd_buf) - sizeof(h)) {
			kvm_err("%s: message too big: %u\n", __func__, h.size);
			err = -E2BIG;
			goto out;
		}
		memcpy(vcpu->sock_cmd_buf, &h, sizeof(h));
		memcpy(vcpu->sock_cmd_buf + sizeof(h), i, h.size);
		vcpu->sock_cmd_ctx = cb_ctx;
		set_sem_req(REQ_CMD, vcpu);
		kvm_make_request(KVM_REQ_INTROSPECTION, vcpu);
		kvm_vcpu_kick(vcpu);
	}

out:
	if (i != (void *) tmp)
		kfree(i);

	if (err) {
		kvm_err("%s: id:%d (%s) err:%d\n", __func__, h.msg_id,
			id2str(h.msg_id), err);
		return false;
	}

	return true;
}

void handle_request(struct kvm_vcpu *vcpu)
{
	struct resp_info *r;
	struct kvmi_socket_hdr h;
	u8 req[960];
	int err;

	memcpy(&h, vcpu->sock_cmd_buf, sizeof(h));
	memcpy(req, vcpu->sock_cmd_buf + sizeof(h), h.size);

	clear_sem_req(REQ_CMD, vcpu);

	r = guest_responses + h.msg_id;
	/* TODO: vcpu->sock_cmd_ctx might be invalid ? */
	err = r->cb(vcpu->sock_cmd_ctx, vcpu->kvm, &h, req);
	if (err)
		kvm_err("%s: id:%d (%s) err:%d\n", __func__, h.msg_id,
			id2str(h.msg_id), err);
}

void kvmi_handle_controller_request(struct kvm_vcpu *vcpu)
{
	while (READ_ONCE(vcpu->pause_count)
	       || READ_ONCE(vcpu->sock_rsp_waiting)
	       || READ_ONCE(vcpu->sem_requests)) {

		down(&vcpu->sock_sem);

		if (test_bit(REQ_PAUSE, &vcpu->sem_requests)) {
			clear_sem_req(REQ_PAUSE, vcpu);
		} else if (test_bit(REQ_RESUME, &vcpu->sem_requests)) {
			clear_sem_req(REQ_RESUME, vcpu);
		} else if (test_bit(REQ_CMD, &vcpu->sem_requests)) {
			handle_request(vcpu);	/* it will clear REQ_CMD bit */
		} else if (test_bit(REQ_REPLY, &vcpu->sem_requests)) {
			clear_sem_req(REQ_REPLY, vcpu);
			WARN_ON(READ_ONCE(vcpu->sock_rsp_waiting) == false);
			WRITE_ONCE(vcpu->sock_rsp_waiting, false);
		} else if (test_bit(REQ_CLOSE, &vcpu->sem_requests)) {
			clear_sem_req(REQ_CLOSE, vcpu);
			break;
		} else {
			WARN_ON(1);
		}
	}
}

bool consume_bytes_from_socket(size_t n, kvmi_socket_read_cb read_cb, void *s)
{
	while (n) {
		u8 buf[128];
		size_t chunk = min(n, sizeof(buf));
		int err = read_cb(s, buf, chunk);

		if (err) {
			kvm_err("%s: read_cb failed: %d\n", __func__, err);
			return false;
		}

		n -= chunk;
	}

	return true;
}

int respond_get_version(void *s, struct kvm *kvm, struct kvmi_socket_hdr *req,
			void *i)
{
	struct {
		struct kvmi_socket_hdr h;
		unsigned int version;
	} resp;

	memset(&resp, 0, sizeof(resp));
	resp.version = KVMI_VERSION;
	return respond_to_request(s, req, &resp, sizeof(resp));
}

int respond_to_request(void *s, struct kvmi_socket_hdr *req, void *buf,
		       size_t size)
{
	struct kvmi_socket_hdr *h = buf;
	struct kvec i = {
		.iov_base = buf,
		.iov_len = size
	};
	int err;

	h->msg_id = req->msg_id | KVMI_FLAG_RESPONSE;
	h->seq = req->seq;
	h->size = (__u16) size - sizeof(*h);

	err = kvmi_socket_send(s, &i, 1, size);

	if (err)
		kvm_err("%s: kvmi_socket_send() => %d\n", __func__, err);

	return err;
}

int respond_to_request_buf(void *s, struct kvmi_socket_hdr *req,
			   const void *buf, size_t size)
{
	struct kvmi_socket_hdr h;
	struct kvec i[2] = {
		{.iov_base = &h, .iov_len = sizeof(h)},
		{.iov_base = (void *) buf, .iov_len = size},
	};
	int err;

	memset(&h, 0, sizeof(h));
	h.msg_id = req->msg_id | KVMI_FLAG_RESPONSE;
	h.seq = req->seq;
	h.size = size;

	err = kvmi_socket_send(s, i, size ? 2 : 1, sizeof(h) + size);

	if (err)
		kvm_err("%s: kvmi_socket_send() => %d\n", __func__, err);

	return err;
}

int respond_get_guests(void *s, struct kvmi_socket_hdr *req)
{
	struct kvm_enum_param p = { };
	u8 *resp;
	size_t resp_size;
	struct kvmi_guests *g;
	int err;

	kvm_enum(cnt_cb, &p.n);

	/* TODO: make struct kvmi_guests easy to use: (size -> cnt, guest[0]) */

	resp_size = sizeof(struct kvmi_socket_hdr) + sizeof(struct kvmi_guests);

	if (p.n)
		resp_size += sizeof(struct kvmi_guest) * (p.n - 1);
	else
		resp_size -= sizeof(struct kvmi_guest);

	resp = kzalloc(resp_size, GFP_KERNEL);

	if (!resp)
		return -ENOMEM;

	g = (struct kvmi_guests *) (resp + sizeof(struct kvmi_socket_hdr));

	if (p.n) {
		p.guests = g;
		kvm_enum(copy_guest_cb, &p);
	}

	g->size = sizeof(g->size) + sizeof(struct kvmi_guest) * p.k;

	err =
	    respond_to_request(s, req, resp,
			       sizeof(struct kvmi_socket_hdr) + g->size);

	kfree(resp);

	return err;
}

int cnt_cb(const struct kvm *kvm, void *param)
{
	unsigned int *n = param;

	if (test_bit(0, &kvm->introduced))
		*n += 1;

	return 0;
}

int copy_guest_cb(const struct kvm *kvm, void *param)
{
	struct kvm_enum_param *p = param;

	if (test_bit(0, &kvm->introduced))
		memcpy(p->guests->guests + p->k++, &kvm->uuid,
		       sizeof(kvm->uuid));

	return (p->k == p->n ? -1 : 0);
}

int respond_get_guest_info(void *s, struct kvm *kvm,
			   struct kvmi_socket_hdr *req, void *i)
{
	struct {
		struct kvmi_socket_hdr h;
		struct kvmi_guest_info m;
	} resp;

	memset(&resp, 0, sizeof(resp));

	resp.m.vcpu_count = atomic_read(&kvm->online_vcpus);

	query_paused_vcpu(kvm, 0, get_tsc_cb, &resp.m.tsc_speed);

	resp.m.tsc_speed *= 1000UL;

	return respond_to_request(s, req, &resp, sizeof(resp));
}

int get_tsc_cb(struct kvm_vcpu *vcpu, void *ctx)
{
	__u64 *tsc = ctx;

	*tsc = vcpu->arch.virtual_tsc_khz;
	return 0;
}

int get_vcpu(struct kvm *kvm, int vcpu_id, struct kvm_vcpu **vcpu)
{
	struct kvm_vcpu *v;

	if (vcpu_id >= atomic_read(&kvm->online_vcpus))
		return -EINVAL;

	v = kvm_get_vcpu(kvm, vcpu_id);

	if (!v)
		return -EINVAL;

	if (vcpu)
		*vcpu = v;

	return 0;
}

int query_paused_vcpu(struct kvm *kvm, int vcpu_id,
		      int (*cb)(struct kvm_vcpu *, void *), void *ctx)
{
	return query_locked_vcpu(kvm, vcpu_id, cb, ctx);
}

int query_locked_vcpu(struct kvm *kvm, int vcpu_id,
		      int (*cb)(struct kvm_vcpu *, void *), void *ctx)
{
	struct kvm_vcpu *vcpu;

	if (vcpu_id >= atomic_read(&kvm->online_vcpus))
		return -EINVAL;

	vcpu = kvm_get_vcpu(kvm, vcpu_id);

	if (!vcpu)
		return -EINVAL;

	return cb(vcpu, ctx);
}

int respond_pause_guest(void *s, struct kvm *kvm, struct kvmi_socket_hdr *req,
			void *i)
{
	return respond_with_error_code(s, vm_pause(kvm), req);
}

int respond_unpause_guest(void *s, struct kvm *kvm, struct kvmi_socket_hdr *req,
			  void *i)
{
	return respond_with_error_code(s, vm_resume(kvm), req);
}

int respond_with_error_code(void *s, int err, struct kvmi_socket_hdr *req)
{
	struct {
		struct kvmi_socket_hdr h;
		int err;
	} resp;

	memset(&resp, 0, sizeof(resp));
	resp.err = err;
	return respond_to_request(s, req, &resp, sizeof(resp));
}

int respond_get_registers(void *s, struct kvm *kvm, struct kvmi_socket_hdr *req,
			  void *i)
{
	struct {
		struct kvmi_socket_hdr h;
		struct kvmi_get_registers_r m;
	} empty;
	struct kvmi_get_registers *r = i;
	struct kvmi_get_registers_r *c = NULL;
	u8 *resp;
	size_t sz_resp;
	__u16 k;
	int err;

	if (req->size < sizeof(*r)
	    || req->size != sizeof(*r) + sizeof(__u32) * r->nmsrs) {
		err = -EINVAL;
		goto out_err;
	}

	sz_resp =
	    sizeof(empty.h) + sizeof(empty.m) +
	    sizeof(struct kvm_msr_entry) * r->nmsrs;

	resp = kzalloc(sz_resp, GFP_KERNEL);

	if (!resp) {
		err = -ENOMEM;
		goto out_err;
	}

	c = (struct kvmi_get_registers_r *) (resp + sizeof(empty.h));
	c->msrs.nmsrs = r->nmsrs;

	for (k = 0; k < r->nmsrs; k++)
		c->msrs.entries[k].index = r->msrs_idx[k];

	err = query_locked_vcpu(kvm, r->vcpu, get_registers_cb, c);

	if (!err) {
		err = respond_to_request(s, req, resp, sz_resp);
		kfree(resp);
		return err;
	}

	kfree(resp);

out_err:
	memset(&empty, 0, sizeof(empty));
	empty.m.err = err;
	respond_to_request(s, req, &empty, sizeof(empty));
	return err;
}

int get_registers_cb(struct kvm_vcpu *vcpu, void *ctx)
{
	struct kvmi_get_registers_r *c = ctx;
	struct kvm_msr_entry *msr = c->msrs.entries + 0;
	unsigned int n = c->msrs.nmsrs;

	for (; n--; msr++) {
		struct msr_data m = {.index = msr->index };
		int err = kvm_get_msr(vcpu, &m);

		if (err)
			return err;

		msr->data = m.data;
	}

	kvm_arch_vcpu_ioctl_get_regs(vcpu, &c->regs);
	kvm_arch_vcpu_ioctl_get_sregs(vcpu, &c->sregs);
	c->mode = kvmi_vcpu_mode(vcpu, &c->sregs);

	return 0;
}

int respond_set_registers(void *s, struct kvm *kvm, struct kvmi_socket_hdr *req,
			  void *i)
{
	struct kvmi_set_registers *r = i;
	int err = query_locked_vcpu(kvm, r->vcpu, set_registers_cb,
				    (void *) &r->regs);

	return respond_with_error_code(s, err, req);
}

int set_registers_cb(struct kvm_vcpu *vcpu, void *ctx)
{
	struct kvm_regs *regs = ctx;

	kvm_arch_vcpu_set_regs(vcpu, regs);
	return 0;
}

int respond_shutdown_guest(void *s, struct kvm *kvm,
			   struct kvmi_socket_hdr *req, void *i)
{
	kvm_vm_shutdown(kvm);
	return 0;
}

int respond_get_mtrr_type(void *s, struct kvm *kvm, struct kvmi_socket_hdr *req,
			  void *i)
{
	struct {
		struct kvmi_socket_hdr h;
		struct kvmi_mtrr_type m;
	} resp;

	memset(&resp, 0, sizeof(resp));
	resp.m.gpa = *((__u64 *) i);
	resp.m.err =
	    query_paused_vcpu(kvm, 0, get_mttr_memory_type_cb, &resp.m);

	return respond_to_request(s, req, &resp, sizeof(resp));
}

int get_mttr_memory_type_cb(struct kvm_vcpu *vcpu, void *ctx)
{
	struct kvmi_mtrr_type *c = ctx;

	c->type = kvm_mtrr_get_guest_memory_type(vcpu, c->gpa);
	return 0;
}

int respond_get_mtrrs(void *s, struct kvm *kvm, struct kvmi_socket_hdr *req,
		      void *i)
{
	struct {
		struct kvmi_socket_hdr h;
		struct kvmi_mtrrs m;
	} resp;

	memset(&resp, 0, sizeof(resp));
	resp.m.vcpu = *((__u16 *) i);
	resp.m.err = query_paused_vcpu(kvm, resp.m.vcpu, get_msr_cb, &resp.m);

	return respond_to_request(s, req, &resp, sizeof(resp));
}

int get_msr_cb(struct kvm_vcpu *vcpu, void *ctx)
{
	struct kvmi_mtrrs *c = ctx;

	if (kvm_mtrr_get_msr(vcpu, MSR_IA32_CR_PAT, &c->pat)
	    || kvm_mtrr_get_msr(vcpu, MSR_MTRRcap, &c->cap)
	    || kvm_mtrr_get_msr(vcpu, MSR_MTRRdefType, &c->type))
		return -EINVAL;

	return 0;
}

int respond_get_xsave_info(void *s, struct kvm *kvm,
			   struct kvmi_socket_hdr *req, void *i)
{
	struct {
		struct kvmi_socket_hdr h;
		struct kvmi_xsave_info m;
	} resp;

	memset(&resp, 0, sizeof(resp));
	resp.m.vcpu = *((__u16 *) i);
	resp.m.err =
	    query_paused_vcpu(kvm, resp.m.vcpu, get_xstate_size_cb,
			      &resp.m.size);

	return respond_to_request(s, req, &resp, sizeof(resp));
}

int get_xstate_size_cb(struct kvm_vcpu *vcpu, void *ctx)
{
	__u64 *size = ctx;

	*size = vcpu->arch.guest_xstate_size;

	return 0;
}

int respond_get_page_access(void *s, struct kvm *kvm,
			    struct kvmi_socket_hdr *req, void *_i)
{
	struct {
		struct kvmi_socket_hdr h;
		struct kvmi_page_access m;
	} resp;
	struct kvmi_page_access *i = _i;

	memset(&resp, 0, sizeof(resp));
	resp.m.vcpu = i->vcpu;	/* ? */
	resp.m.gpa = i->gpa;
	resp.m.err = query_paused_vcpu(kvm, i->vcpu, get_page_info_cb, &resp.m);

	return respond_to_request(s, req, &resp, sizeof(resp));
}

int get_page_info_cb(struct kvm_vcpu *vcpu, void *ctx)
{
	struct kvmi_page_access *c = ctx;

	c->access = kvm_mmu_get_spte(vcpu->kvm, vcpu, c->gpa);

	return 0;
}

int respond_set_page_access(void *s, struct kvm *kvm,
			    struct kvmi_socket_hdr *req, void *_i)
{
	int err;
	struct kvmi_page_access *i = _i;

	if (i->access & ~7ULL) {
		err = -EINVAL;
	} else {
		err =
		    query_paused_vcpu(kvm, i->vcpu, set_page_info_cb,
				      (void *) i);
	}

	return respond_with_error_code(s, err, req);
}

int set_page_info_cb(struct kvm_vcpu *vcpu, void *ctx)
{
	struct kvmi_page_access *c = ctx;

	return kvmi_set_mem_access(vcpu->kvm, c->gpa, c->access);
}

int respond_inject_page_fault(void *s, struct kvm *kvm,
			      struct kvmi_socket_hdr *req, void *_i)
{
	struct kvmi_page_fault *i = _i;
	int err;

	err = query_paused_vcpu(kvm, i->vcpu, inject_pf_cb, i);

	return respond_with_error_code(s, err, req);
}

int inject_pf_cb(struct kvm_vcpu *vcpu, void *ctx)
{
	struct kvmi_page_fault *c = ctx;
	struct x86_exception fault = {
		.address = c->gva,
		.error_code = c->error
	};

	kvm_inject_page_fault(vcpu, &fault);

	/*
	 * Generate an event to let the client know if the injection
	 * worked
	 */
	atomic_set(&vcpu->arch.next_interrupt_enabled, 1);
	return 0;
}

static unsigned long gfn_to_hva_safe(struct kvm *kvm, gfn_t gfn)
{
	unsigned long hva;

	mutex_lock(&kvm->slots_lock);
	hva = gfn_to_hva(kvm, gfn);
	mutex_unlock(&kvm->slots_lock);
	return hva;
}

static long get_user_pages_remote_unlocked(struct mm_struct *mm,
					   unsigned long start,
					   unsigned long nr_pages,
					   unsigned int gup_flags,
					   struct page **pages)
{
	long ret;
	struct task_struct *tsk = NULL;
	struct vm_area_struct **vmas = NULL;
	int locked = 1;

	down_read(&mm->mmap_sem);
	ret =
	    get_user_pages_remote(tsk, mm, start, nr_pages, gup_flags, pages,
				  vmas, &locked);
	if (locked)
		up_read(&mm->mmap_sem);
	return ret;
}

int respond_read_physical(void *s, struct kvm *kvm, struct kvmi_socket_hdr *req,
			  void *_i)
{
	struct kvmi_rw_physical_info *i = _i;
	int err;
	unsigned long hva;
	struct page *page;
	void *ptr;
	struct kvm_vcpu *vcpu;

	if (!i->size || i->size > PAGE_SIZE) {
		err = -EINVAL;
		goto out_err_no_resume;
	}

	err = get_vcpu(kvm, 0, &vcpu);

	if (err)
		goto out_err_no_resume;

	vm_pause(kvm);

	hva = gfn_to_hva_safe(kvm, gpa_to_gfn(i->gpa));

	if (kvm_is_error_hva(hva)) {
		err = -EFAULT;
		goto out_err;
	}

	if (((i->gpa & ~PAGE_MASK) + i->size) > PAGE_SIZE) {
		err = -EINVAL;
		goto out_err;
	}

	err = get_user_pages_remote_unlocked(kvm->mm, hva, 1, 0, &page);
	if (err != 1) {
		err = -EFAULT;
		goto out_err;
	}

	ptr = kmap_atomic(page);

	err =
	    respond_to_request_buf(s, req, ptr + (i->gpa & ~PAGE_MASK),
				   i->size);

	kunmap_atomic(ptr);
	put_page(page);

	vm_resume(kvm);

	return err;

out_err:
	vm_resume(kvm);

out_err_no_resume:
	return respond_to_request_buf(s, req, NULL, 0);
}

int respond_write_physical(void *s, struct kvm *kvm,
			   struct kvmi_socket_hdr *req, void *_i)
{
	struct kvmi_rw_physical_info *i = _i;
	int err;
	unsigned long hva;
	struct page *page;
	void *ptr;
	struct kvm_vcpu *vcpu;

	if (req->size != sizeof(struct kvmi_rw_physical_info) + i->size) {
		err = -EINVAL;
		goto out_err_no_resume;
	}

	if (!i->size || i->size > PAGE_SIZE) {
		err = -EINVAL;
		goto out_err_no_resume;
	}

	err = get_vcpu(kvm, 0, &vcpu);

	if (err)
		goto out_err_no_resume;

	vm_pause(kvm);

	hva = gfn_to_hva_safe(kvm, gpa_to_gfn(i->gpa));

	if (kvm_is_error_hva(hva)) {
		err = -EFAULT;
		goto out_err;
	}

	if (((i->gpa & ~PAGE_MASK) + i->size) > PAGE_SIZE) {
		err = -EINVAL;
		goto out_err;
	}

	err =
	    get_user_pages_remote_unlocked(kvm->mm, hva, 1, FOLL_WRITE, &page);
	if (err != 1) {
		err = -EFAULT;
		goto out_err;
	}

	ptr = kmap_atomic(page);

	memcpy(ptr + (i->gpa & ~PAGE_MASK), (i + 1), i->size);

	kunmap_atomic(ptr);
	put_page(page);

	err = 0;

out_err:
	vm_resume(kvm);

out_err_no_resume:
	return respond_with_error_code(s, err, req);
}

static struct vm_area_struct *get_one_page_vma(struct kvm *kvm,
					       unsigned long addr)
{
	struct vm_area_struct *v =
	    find_vma_intersection(kvm->mm, addr, addr + PAGE_SIZE);

	if (!v) {
		kvm_err("%s: find_vma(%lX) = NULL\n", __func__, addr);
		return NULL;
	}

	if (addr != v->vm_start) {
		int err = split_vma(kvm->mm, v, addr, 0);

		if (err) {
			kvm_err("%s: split_vma(cut above): %d\n", __func__,
				err);
			return NULL;
		}
		v = find_vma(kvm->mm, addr);
	}

	if (v->vm_end - v->vm_start != PAGE_SIZE) {
		int err = split_vma(kvm->mm, v, addr + PAGE_SIZE, 0);

		if (err) {
			kvm_err("%s: split_vma(cut below): %d\n", __func__,
				err);
			return NULL;
		}
	}

	return v;
}

int respond_map_physical_page_to_sva(void *s, struct kvm *kvm,
				     struct kvmi_socket_hdr *req, void *_i)
{
	struct kvmi_map_physical_to_sva_info *i = _i;
	int err;
	unsigned long hva_src, hva_dest;
	struct vm_area_struct *vma_dest;
	struct page *page;
	struct kvm_vcpu *vcpu;

	err = get_vcpu(kvm, 0, &vcpu);

	if (err)
		goto out_err_no_resume;

	vm_pause(kvm);

	hva_src = gfn_to_hva_safe(kvm, gpa_to_gfn(i->gpa_src));
	hva_dest = gfn_to_hva_safe(sva, i->gfn_dest);

	if (kvm_is_error_hva(hva_src) || kvm_is_error_hva(hva_dest)) {
		err = -EFAULT;
		goto out_err;
	}

	if (get_user_pages_remote_unlocked
	    (kvm->mm, hva_src, 1, FOLL_WRITE, &page) != 1) {
		err = -ENOENT;
		goto out_err;
	}

	down_write(&sva->mm->mmap_sem);
	vma_dest = get_one_page_vma(sva, hva_dest);
	if (vma_dest) {
		err = vm_replace_page(vma_dest, page);
		if (err)
			kvm_err("%s: vm_replace_page: %d\n", __func__, err);
	} else
		err = -ENOENT;
	up_write(&sva->mm->mmap_sem);

	put_page(page);

out_err:
	vm_resume(kvm);

out_err_no_resume:
	if (err)
		kvm_err("%s: %d\n", __func__, err);

	return respond_with_error_code(s, err, req);
}

int respond_unmap_physical_page_from_sva(void *s, struct kvm *kvm,
					 struct kvmi_socket_hdr *req, void *_i)
{
	int err;
	unsigned long hva;
	struct kvmi_unmap_physical_from_sva_info *i = _i;
	struct vm_area_struct *vma;
	struct page *page;
	struct kvm_vcpu *vcpu;

	err = get_vcpu(kvm, 0, &vcpu);

	if (err)
		goto out_err_no_resume;

	vm_pause(kvm);

	page = alloc_page(GFP_HIGHUSER_MOVABLE);
	if (!page) {
		err = -ENOMEM;
		goto out_err;
	}

	hva = gfn_to_hva_safe(sva, i->gfn_dest);
	if (kvm_is_error_hva(hva)) {
		err = -EFAULT;
		goto out_err;
	}

	down_write(&sva->mm->mmap_sem);

	vma = find_vma(sva->mm, hva);
	if (vma->vm_start != hva
			|| (vma->vm_end - vma->vm_start) != PAGE_SIZE) {
		kvm_err("%s: invalid vma\n", __func__);
		err = -EINVAL;
	} else {
		err = vm_replace_page(vma, page);
		if (err)
			kvm_err("%s: vm_replace_page: %d\n", __func__, err);
		else
			put_page(page);
	}

	up_write(&sva->mm->mmap_sem);

out_err:
	if (err) {
		if (page)
			__free_pages(page, 0);
		kvm_err("%s: %d\n", __func__, err);
	}

	vm_resume(kvm);

out_err_no_resume:

	return respond_with_error_code(s, err, req);
}

int respond_event_control(void *s, struct kvm *kvm, struct kvmi_socket_hdr *req,
			  void *_i)
{
	struct kvmi_event_control *i = _i;
	int err;
	struct kvm_vcpu *vcpu;

	if (i->events & ~KVMI_KNOWN_EVENTS) {
		err = -EINVAL;
		goto out_err;
	}

	err = get_vcpu(kvm, i->vcpu, &vcpu);

	if (err)
		goto out_err;

	if (i->events & KVMI_EVENT_BREAKPOINT) {
		unsigned int event_mask = atomic_read(&kvm->event_mask);

		if (!(event_mask & KVMI_EVENT_BREAKPOINT)) {
			struct kvm_guest_debug dbg = { };

			dbg.control =
			    KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_SW_BP;

			err = kvm_arch_vcpu_ioctl_set_guest_debug(vcpu, &dbg);
		}
	}

	if (!err)
		atomic_set(&kvm->event_mask, i->events);

out_err:
	return respond_with_error_code(s, err, req);
}

int respond_cr_control(void *s, struct kvm *kvm, struct kvmi_socket_hdr *req,
		       void *i)
{
	int err = query_paused_vm(kvm, set_cr_control, i);

	return respond_with_error_code(s, err, req);
}

int set_cr_control(struct kvm *kvm, void *ctx)
{
	struct kvmi_cr_control *i = ctx;

	switch (i->cr) {
	case 0:
	case 3:
	case 4:
		if (i->enable)
			set_bit(i->cr, &kvm->cr_mask);
		else
			clear_bit(i->cr, &kvm->cr_mask);
		return 0;

	default:
		return -EINVAL;
	}
}

int respond_msr_control(void *s, struct kvm *kvm, struct kvmi_socket_hdr *req,
			void *i)
{
	int err = query_paused_vm(kvm, set_msr_control, i);

	return respond_with_error_code(s, err, req);
}

int query_paused_vm(struct kvm *kvm, int (*cb)(struct kvm *kvm, void *),
		    void *ctx)
{
	struct kvm_vcpu *vcpu;
	int err;

	err = get_vcpu(kvm, 0, &vcpu);
	if (err) {
		kvm_err("%s: get_vcpu: %d\n", __func__, err);
		return err;
	}

	vm_pause(kvm);

	err = cb(kvm, ctx);

	vm_resume(kvm);

	return err;
}

int set_msr_control(struct kvm *kvm, void *ctx)
{
	struct kvmi_msr_control *i = ctx;

	int err = msr_control(kvm, i->msr, i->enable);

	if (!err)
		kvm_arch_msr_intercept(i->msr, i->enable);

	return err;
}

int respond_inject_breakpoint(void *s, struct kvm *kvm,
			      struct kvmi_socket_hdr *req, void *i)
{
	int err;

	err =
	    query_locked_vcpu(kvm, *((__u16 *) i), inject_breakpoint_cb, NULL);

	return respond_with_error_code(s, err, req);
}

int inject_breakpoint_cb(struct kvm_vcpu *vcpu, void *ctx)
{
	struct kvm_guest_debug dbg = {.control = KVM_GUESTDBG_INJECT_BP };

	int err = kvm_arch_vcpu_ioctl_set_guest_debug(vcpu, &dbg);

	/*
	 * Generate an event to let the client know if the injection
	 * worked
	 */

	/* if (!err) */
	atomic_set(&vcpu->arch.next_interrupt_enabled, 1);
	return err;
}

void send_event(struct kvm *kvm, int msg_id, void *data, size_t size)
{
	struct kvmi_socket_hdr h;
	struct kvec i[2] = {
		{.iov_base = &h, .iov_len = sizeof(h)},
		{.iov_base = (void *) data, .iov_len = size}
	};
	size_t n = size ? 2 : 1;
	size_t total = sizeof(h) + size;

	memset(&h, 0, sizeof(h));
	h.msg_id = msg_id;
	h.seq = new_seq();
	h.size = size;

	send_async_event_to_socket(kvm, i, n, total);
}

u32 new_seq(void)
{
	return atomic_inc_return(&seq_ev);
}

static const char *event_str(unsigned int e)
{
	switch (e) {
	case KVMI_EVENT_CR:
		return "CR";
	case KVMI_EVENT_MSR:
		return "MSR";
	case KVMI_EVENT_XSETBV:
		return "XSETBV";
	case KVMI_EVENT_BREAKPOINT:
		return "BREAKPOINT";
	case KVMI_EVENT_USER_CALL:
		return "USER_CALL";
	case KVMI_EVENT_PAGE_FAULT:
		return "PAGE_FAULT";
	case KVMI_EVENT_TRAP:
		return "TRAP";
	default:
		return "EVENT?";
	}
}

static void inspect_kvmi_event(struct kvmi_event *ev, u32 seq)
{
	switch (ev->event) {
	case KVMI_EVENT_CR:
		kvm_debug("%s: seq:%u %-11s(%d) cr:%x old:%llx new:%llx\n",
			  __func__, seq, event_str(ev->event), ev->vcpu,
			  ev->cr.cr, ev->cr.old_value, ev->cr.new_value);
		break;
	case KVMI_EVENT_MSR:
		kvm_debug("%s: seq:%u %-11s(%d) msr:%x old:%llx new:%llx\n",
			  __func__, seq, event_str(ev->event), ev->vcpu,
			  ev->msr.msr, ev->msr.old_value, ev->msr.new_value);
		break;
	case KVMI_EVENT_XSETBV:
		kvm_debug("%s: seq:%u %-11s(%d) xcr0:%llx\n", __func__, seq,
			  event_str(ev->event), ev->vcpu, ev->xsetbv.xcr0);
		break;
	case KVMI_EVENT_BREAKPOINT:
		kvm_debug("%s: seq:%u %-11s(%d) gpa:%llx\n", __func__, seq,
			  event_str(ev->event), ev->vcpu, ev->breakpoint.gpa);
		break;
	case KVMI_EVENT_USER_CALL:
		kvm_debug("%s: seq:%u %-11s(%d)\n", __func__, seq,
			  event_str(ev->event), ev->vcpu);
		break;
	case KVMI_EVENT_PAGE_FAULT:
		kvm_debug("%s: seq:%u %-11s(%d) gpa:%llx gva:%llx mode:%x\n",
			  __func__, seq, event_str(ev->event), ev->vcpu,
			  ev->page_fault.gpa, ev->page_fault.gva,
			  ev->page_fault.mode);
		break;
	case KVMI_EVENT_TRAP:
		kvm_debug
		    ("%s: seq:%u %-11s(%d) vector:%x type:%x err:%x cr2:%llx\n",
		     __func__, seq, event_str(ev->event), ev->vcpu,
		     ev->trap.vector, ev->trap.type, ev->trap.err,
		     ev->trap.cr2);
		break;
	}
}

bool send_vcpu_event_and_wait(struct kvm_vcpu *vcpu, void *ev, size_t ev_size,
			      void *resp, size_t resp_size)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvmi_socket_hdr h;
	struct kvec i[2] = {
		{.iov_base = &h, .iov_len = sizeof(h)},
		{.iov_base = ev, .iov_len = ev_size}
	};
	size_t total = sizeof(h) + ev_size;
	struct kvmi_event *e = ev;
	bool ok = false;

	memset(&h, 0, sizeof(h));
	h.msg_id = KVMI_EVENT_VCPU;
	h.seq = new_seq();
	h.size = ev_size;

	inspect_kvmi_event(e, h.seq);

	vcpu->sock_rsp_buf = resp;
	vcpu->sock_rsp_size = resp_size;
	vcpu->sock_rsp_seq = h.seq;
	WRITE_ONCE(vcpu->sock_rsp_received, 0);
	WRITE_ONCE(vcpu->sock_rsp_waiting, true);

	if (send_async_event_to_socket(kvm, i, 2, total) == 0)
		kvmi_handle_controller_request(vcpu);

	kvm_debug("%s: reply for vcpu:%d event:%d (%s)\n", __func__, e->vcpu,
		  e->event, event_str(e->event));

	ok = (READ_ONCE(vcpu->sock_rsp_received) > 0);
	return ok;
}

int send_async_event_to_socket(struct kvm *kvm, struct kvec *i, size_t n,
			       size_t bytes)
{
	int err;

	read_lock(&kvm->socket_ctx_lock);

	if (kvm->socket_ctx)
		err = kvmi_socket_send(kvm->socket_ctx, i, n, bytes);
	else
		err = -ENOENT;

	read_unlock(&kvm->socket_ctx_lock);

	if (err)
		kvm_err("%s: kvmi_socket_send() => %d\n", __func__, err);

	return err;
}

void wakeup_events(struct kvm *kvm)
{
	int i;
	struct kvm_vcpu *vcpu;

	mutex_lock(&kvm->lock);
	kvm_for_each_vcpu(i, vcpu, kvm) {
		set_sem_req(REQ_CLOSE, vcpu);
		while (test_bit(REQ_CLOSE, &vcpu->sem_requests))
			;
	}
	mutex_unlock(&kvm->lock);
}

void __release_kvm_socket(struct kvm *kvm)
{
	if (kvm->socket_ctx) {
		kvmi_socket_release(kvm->socket_ctx);
		kvm->socket_ctx = NULL;
	}
}

int kvmi_patch_emul_instr(struct kvm_vcpu *vcpu, void *val, unsigned int bytes)
{
	u32 size;

	if (bytes > vcpu->ctx_size) {
		kvm_err("%s: requested %u bytes(s) but only %u available\n",
			__func__, bytes, vcpu->ctx_size);
		return X86EMUL_UNHANDLEABLE;
	}
	size = min(vcpu->ctx_size, bytes);
	memcpy(val, &vcpu->ctx_data[vcpu->ctx_pos], size);
	vcpu->ctx_size -= size;
	vcpu->ctx_pos += size;
	return X86EMUL_CONTINUE;

}
