// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2017-2019, IBM Corporation.
 */

#define pr_fmt(fmt) "xive-kvm: " fmt

#include <linux/anon_inodes.h>
#include <linux/kernel.h>
#include <linux/kvm_host.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/percpu.h>
#include <linux/cpumask.h>
#include <asm/uaccess.h>
#include <asm/kvm_book3s.h>
#include <asm/kvm_ppc.h>
#include <asm/hvcall.h>
#include <asm/xics.h>
#include <asm/xive.h>
#include <asm/xive-regs.h>
#include <asm/debug.h>
#include <asm/debugfs.h>
#include <asm/time.h>
#include <asm/opal.h>

#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include "book3s_xive.h"

static void xive_native_cleanup_queue(struct kvm_vcpu *vcpu, int prio)
{
	struct kvmppc_xive_vcpu *xc = vcpu->arch.xive_vcpu;
	struct xive_q *q = &xc->queues[prio];

	xive_native_disable_queue(xc->vp_id, q, prio);
	if (q->qpage) {
		put_page(virt_to_page(q->qpage));
		q->qpage = NULL;
	}
}

void kvmppc_xive_native_cleanup_vcpu(struct kvm_vcpu *vcpu)
{
	struct kvmppc_xive_vcpu *xc = vcpu->arch.xive_vcpu;
	int i;

	if (!kvmppc_xive_enabled(vcpu))
		return;

	if (!xc)
		return;

	pr_devel("native_cleanup_vcpu(cpu=%d)\n", xc->server_num);

	/* Ensure no interrupt is still routed to that VP */
	xc->valid = false;
	kvmppc_xive_disable_vcpu_interrupts(vcpu);

	/* Disable the VP */
	xive_native_disable_vp(xc->vp_id);

	/* Free the queues & associated interrupts */
	for (i = 0; i < KVMPPC_XIVE_Q_COUNT; i++) {
		/* Free the escalation irq */
		if (xc->esc_virq[i]) {
			free_irq(xc->esc_virq[i], vcpu);
			irq_dispose_mapping(xc->esc_virq[i]);
			kfree(xc->esc_virq_names[i]);
			xc->esc_virq[i] = 0;
		}

		/* Free the queue */
		xive_native_cleanup_queue(vcpu, i);
	}

	/* Free the VP */
	kfree(xc);

	/* Cleanup the vcpu */
	vcpu->arch.irq_type = KVMPPC_IRQ_DEFAULT;
	vcpu->arch.xive_vcpu = NULL;
}

int kvmppc_xive_native_connect_vcpu(struct kvm_device *dev,
				    struct kvm_vcpu *vcpu, u32 cpu)
{
	struct kvmppc_xive *xive = dev->private;
	struct kvmppc_xive_vcpu *xc;
	int rc;

	pr_devel("native_connect_vcpu(cpu=%d)\n", cpu);

	if (dev->ops != &kvm_xive_native_ops) {
		pr_devel("Wrong ops !\n");
		return -EPERM;
	}
	if (xive->kvm != vcpu->kvm)
		return -EPERM;
	if (vcpu->arch.irq_type)
		return -EBUSY;
	if (kvmppc_xive_find_server(vcpu->kvm, cpu)) {
		pr_devel("Duplicate !\n");
		return -EEXIST;
	}
	if (cpu >= KVM_MAX_VCPUS) {
		pr_devel("Out of bounds !\n");
		return -EINVAL;
	}
	xc = kzalloc(sizeof(*xc), GFP_KERNEL);
	if (!xc)
		return -ENOMEM;

	mutex_lock(&vcpu->kvm->lock);
	vcpu->arch.xive_vcpu = xc;
	xc->xive = xive;
	xc->vcpu = vcpu;
	xc->server_num = cpu;
	xc->vp_id = xive->vp_base + cpu;
	xc->valid = true;

	rc = xive_native_get_vp_info(xc->vp_id, &xc->vp_cam, &xc->vp_chip_id);
	if (rc) {
		pr_err("Failed to get VP info from OPAL: %d\n", rc);
		goto bail;
	}

	/*
	 * Enable the VP first as the single escalation mode will
	 * affect escalation interrupts numbering
	 */
	rc = xive_native_enable_vp(xc->vp_id, xive->single_escalation);
	if (rc) {
		pr_err("Failed to enable VP in OPAL: %d\n", rc);
		goto bail;
	}

	/* Configure VCPU fields for use by assembly push/pull */
	vcpu->arch.xive_saved_state.w01 = cpu_to_be64(0xff000000);
	vcpu->arch.xive_cam_word = cpu_to_be32(xc->vp_cam | TM_QW1W2_VO);

	/* TODO: initialize queues ? */

bail:
	vcpu->arch.irq_type = KVMPPC_IRQ_XIVE;
	mutex_unlock(&vcpu->kvm->lock);
	if (rc)
		kvmppc_xive_native_cleanup_vcpu(vcpu);

	return rc;
}

static int kvmppc_xive_native_set_attr(struct kvm_device *dev,
				       struct kvm_device_attr *attr)
{
	return -ENXIO;
}

static int kvmppc_xive_native_get_attr(struct kvm_device *dev,
				       struct kvm_device_attr *attr)
{
	return -ENXIO;
}

static int kvmppc_xive_native_has_attr(struct kvm_device *dev,
				       struct kvm_device_attr *attr)
{
	return -ENXIO;
}

static void kvmppc_xive_native_free(struct kvm_device *dev)
{
	struct kvmppc_xive *xive = dev->private;
	struct kvm *kvm = xive->kvm;
	int i;

	debugfs_remove(xive->dentry);

	pr_devel("Destroying xive native for partition\n");

	if (kvm)
		kvm->arch.xive = NULL;

	/* Mask and free interrupts */
	for (i = 0; i <= xive->max_sbid; i++) {
		if (xive->src_blocks[i])
			kvmppc_xive_free_sources(xive->src_blocks[i]);
		kfree(xive->src_blocks[i]);
		xive->src_blocks[i] = NULL;
	}

	if (xive->vp_base != XIVE_INVALID_VP)
		xive_native_free_vp_block(xive->vp_base);

	kfree(xive);
	kfree(dev);
}

static int kvmppc_xive_native_create(struct kvm_device *dev, u32 type)
{
	struct kvmppc_xive *xive;
	struct kvm *kvm = dev->kvm;
	int ret = 0;

	pr_devel("Creating xive native for partition\n");

	if (kvm->arch.xive)
		return -EEXIST;

	xive = kzalloc(sizeof(*xive), GFP_KERNEL);
	if (!xive)
		return -ENOMEM;

	dev->private = xive;
	xive->dev = dev;
	xive->kvm = kvm;
	kvm->arch.xive = xive;

	/* We use the default queue size set by the host */
	xive->q_order = xive_native_default_eq_shift();
	if (xive->q_order < PAGE_SHIFT)
		xive->q_page_order = 0;
	else
		xive->q_page_order = xive->q_order - PAGE_SHIFT;

	/* Allocate a bunch of VPs */
	xive->vp_base = xive_native_alloc_vp_block(KVM_MAX_VCPUS);
	pr_devel("VP_Base=%x\n", xive->vp_base);

	if (xive->vp_base == XIVE_INVALID_VP)
		ret = -ENOMEM;

	xive->single_escalation = xive_native_has_single_escalation();

	if (ret)
		kfree(xive);

	return ret;
}

static int xive_native_debug_show(struct seq_file *m, void *private)
{
	struct kvmppc_xive *xive = m->private;
	struct kvm *kvm = xive->kvm;
	struct kvm_vcpu *vcpu;
	unsigned int i;

	if (!kvm)
		return 0;

	seq_puts(m, "=========\nVCPU state\n=========\n");

	kvm_for_each_vcpu(i, vcpu, kvm) {
		struct kvmppc_xive_vcpu *xc = vcpu->arch.xive_vcpu;

		if (!xc)
			continue;

		seq_printf(m, "cpu server %#x NSR=%02x CPPR=%02x IBP=%02x PIPR=%02x w01=%016llx w2=%08x\n",
			   xc->server_num,
			   vcpu->arch.xive_saved_state.nsr,
			   vcpu->arch.xive_saved_state.cppr,
			   vcpu->arch.xive_saved_state.ipb,
			   vcpu->arch.xive_saved_state.pipr,
			   vcpu->arch.xive_saved_state.w01,
			   (u32) vcpu->arch.xive_cam_word);

		kvmppc_xive_debug_show_queues(m, vcpu);
	}

	return 0;
}

static int xive_native_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, xive_native_debug_show, inode->i_private);
}

static const struct file_operations xive_native_debug_fops = {
	.open = xive_native_debug_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void xive_native_debugfs_init(struct kvmppc_xive *xive)
{
	char *name;

	name = kasprintf(GFP_KERNEL, "kvm-xive-%p", xive);
	if (!name) {
		pr_err("%s: no memory for name\n", __func__);
		return;
	}

	xive->dentry = debugfs_create_file(name, 0444, powerpc_debugfs_root,
					   xive, &xive_native_debug_fops);

	pr_debug("%s: created %s\n", __func__, name);
	kfree(name);
}

static void kvmppc_xive_native_init(struct kvm_device *dev)
{
	struct kvmppc_xive *xive = (struct kvmppc_xive *)dev->private;

	/* Register some debug interfaces */
	xive_native_debugfs_init(xive);
}

struct kvm_device_ops kvm_xive_native_ops = {
	.name = "kvm-xive-native",
	.create = kvmppc_xive_native_create,
	.init = kvmppc_xive_native_init,
	.destroy = kvmppc_xive_native_free,
	.set_attr = kvmppc_xive_native_set_attr,
	.get_attr = kvmppc_xive_native_get_attr,
	.has_attr = kvmppc_xive_native_has_attr,
};

void kvmppc_xive_native_init_module(void)
{
	;
}

void kvmppc_xive_native_exit_module(void)
{
	;
}
