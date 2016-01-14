#define pr_fmt(fmt) "kcov: " fmt

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/kcov.h>

enum kcov_mode {
	/*
	 * Tracing coverage collection mode.
	 * Covered PCs are collected in a per-task buffer.
	 */
	kcov_mode_trace = 1,
};

/*
 * kcov descriptor (one per opened debugfs file).
 * State transitions of the descriptor:
 *  - initial state after open()
 *  - then there must be a single ioctl(KCOV_INIT_TRACE) call
 *  - then, mmap() call (several calls are allowed but not useful)
 *  - then, repeated enable/disable for a task (only one task a time allowed)
 */
struct kcov {
	/*
	 * Reference counter. We keep one for:
	 *  - opened file descriptor
	 *  - mmapped region (including copies after fork)
	 *  - task with enabled coverage (we can't unwire it from another task)
	 */
	atomic_t		rc;
	/* The lock protects mode, size, area and t. */
	spinlock_t		lock;
	enum kcov_mode		mode;
	unsigned		size;
	void			*area;
	struct task_struct	*t;
};

/*
 * Entry point from instrumented code.
 * This is called once per basic-block/edge.
 */
void __sanitizer_cov_trace_pc(void)
{
	struct task_struct *t;
	enum kcov_mode mode;

	t = current;
	/*
	 * We are interested in code coverage as a function of a syscall inputs,
	 * so we ignore code executed in interrupts.
	 */
	if (!t || in_interrupt())
		return;
	mode = READ_ONCE(t->kcov_mode);
	if (mode == kcov_mode_trace) {
		u32 *area;
		u32 pos;

		/*
		 * There is some code that runs in interrupts but for which
		 * in_interrupt() returns false (e.g. preempt_schedule_irq()).
		 * READ_ONCE()/barrier() effectively provides load-acquire wrt
		 * interrupts, there are paired barrier()/WRITE_ONCE() in
		 * kcov_ioctl_locked().
		 */
		barrier();
		area = t->kcov_area;
		/* The first u32 is number of subsequent PCs. */
		pos = READ_ONCE(area[0]) + 1;
		if (likely(pos < t->kcov_size)) {
			area[pos] = (u32)_RET_IP_;
			WRITE_ONCE(area[0], pos);
		}
	}
}
EXPORT_SYMBOL(__sanitizer_cov_trace_pc);

static void kcov_get(struct kcov *kcov)
{
	atomic_inc(&kcov->rc);
}

static void kcov_put(struct kcov *kcov)
{
	if (atomic_dec_and_test(&kcov->rc)) {
		vfree(kcov->area);
		kfree(kcov);
	}
}

void kcov_task_init(struct task_struct *t)
{
	t->kcov_mode = 0;
	t->kcov_size = 0;
	t->kcov_area = NULL;
	t->kcov = NULL;
}

void kcov_task_exit(struct task_struct *t)
{
	struct kcov *kcov;

	kcov = t->kcov;
	if (kcov == NULL)
		return;
	spin_lock(&kcov->lock);
	if (WARN_ON(kcov->t != t)) {
		spin_unlock(&kcov->lock);
		return;
	}
	/* Just to not leave dangling references behind. */
	kcov_task_init(t);
	kcov->t = NULL;
	spin_unlock(&kcov->lock);
	kcov_put(kcov);
}

static int kcov_vm_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct kcov *kcov;
	unsigned long off;
	struct page *page;

	/* Map the preallocated kcov->area. */
	kcov = vma->vm_file->private_data;
	off = vmf->pgoff << PAGE_SHIFT;
	if (off >= kcov->size * sizeof(u32))
		return VM_FAULT_SIGSEGV;

	page = vmalloc_to_page(kcov->area + off);
	get_page(page);
	vmf->page = page;
	return 0;
}

static void kcov_unmap(struct vm_area_struct *vma)
{
	kcov_put(vma->vm_file->private_data);
}

static void kcov_map_copied(struct vm_area_struct *vma)
{
	struct kcov *kcov;

	kcov = vma->vm_file->private_data;
	kcov_get(kcov);
}

static const struct vm_operations_struct kcov_vm_ops = {
	.fault = kcov_vm_fault,
	.close = kcov_unmap,
	/* Called on fork()/clone() when the mapping is copied. */
	.open  = kcov_map_copied,
};

static int kcov_mmap(struct file *filep, struct vm_area_struct *vma)
{
	int res = 0;
	void *area;
	struct kcov *kcov = vma->vm_file->private_data;

	area = vmalloc_user(vma->vm_end - vma->vm_start);
	if (!area)
		return -ENOMEM;

	spin_lock(&kcov->lock);
	if (kcov->mode == 0 || vma->vm_pgoff != 0 ||
	    vma->vm_end - vma->vm_start != kcov->size * sizeof(u32)) {
		res = -EINVAL;
		goto exit;
	}
	if (!kcov->area) {
		kcov->area = area;
		area = NULL;
	}
	/*
	 * The file drops a reference on close, but the file
	 * descriptor can be closed with the mmaping still alive so we keep
	 * a reference for those.  This is put in kcov_unmap().
	 */
	kcov_get(kcov);
	vma->vm_ops = &kcov_vm_ops;
exit:
	spin_unlock(&kcov->lock);
	vfree(area);
	return res;
}

static int kcov_open(struct inode *inode, struct file *filep)
{
	struct kcov *kcov;

	kcov = kzalloc(sizeof(*kcov), GFP_KERNEL);
	if (!kcov)
		return -ENOMEM;
	atomic_set(&kcov->rc, 1);
	spin_lock_init(&kcov->lock);
	filep->private_data = kcov;
	return nonseekable_open(inode, filep);
}

static int kcov_close(struct inode *inode, struct file *filep)
{
	kcov_put(filep->private_data);
	return 0;
}

static int kcov_ioctl_locked(struct kcov *kcov, unsigned int cmd,
			     unsigned long arg)
{
	struct task_struct *t;

	switch (cmd) {
	case KCOV_INIT_TRACE:
		/*
		 * Enable kcov in trace mode and setup buffer size.
		 * Must happen before anything else.
		 * Size must be at least 2 to hold current position and one PC.
		 */
		if (arg < 2 || arg > INT_MAX)
			return -EINVAL;
		if (kcov->mode != 0)
			return -EBUSY;
		kcov->mode = kcov_mode_trace;
		kcov->size = arg;
		return 0;
	case KCOV_ENABLE:
		/*
		 * Enable coverage for the current task.
		 * At this point user must have been enabled trace mode,
		 * and mmapped the file. Coverage collection is disabled only
		 * at task exit or voluntary by KCOV_DISABLE. After that it can
		 * be enabled for another task.
		 */
		if (kcov->mode == 0 || kcov->area == NULL)
			return -EINVAL;
		if (kcov->t != NULL)
			return -EBUSY;
		t = current;
		/* Cache in task struct for performance. */
		t->kcov_size = kcov->size;
		t->kcov_area = kcov->area;
		/* See comment in __sanitizer_cov_trace_pc(). */
		barrier();
		WRITE_ONCE(t->kcov_mode, kcov->mode);
		t->kcov = kcov;
		kcov->t = t;
		/* This is put either in kcov_task_exit() or in KCOV_DISABLE. */
		kcov_get(kcov);
		return 0;
	case KCOV_DISABLE:
		/* Disable coverage for the current task. */
		if (current->kcov != kcov)
			return -EINVAL;
		t = current;
		if (WARN_ON(kcov->t != t))
			return -EINVAL;
		kcov_task_init(t);
		kcov->t = NULL;
		kcov_put(kcov);
		return 0;
	default:
		return -EINVAL;
	}
}

static long kcov_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct kcov *kcov;
	int res;

	kcov = filep->private_data;
	spin_lock(&kcov->lock);
	res = kcov_ioctl_locked(kcov, cmd, arg);
	spin_unlock(&kcov->lock);
	return res;
}

static const struct file_operations kcov_fops = {
	.open		= kcov_open,
	.unlocked_ioctl	= kcov_ioctl,
	.mmap		= kcov_mmap,
	.release        = kcov_close,
};

static int __init kcov_init(void)
{
	if (!debugfs_create_file("kcov", 0666, NULL, NULL, &kcov_fops)) {
		pr_err("init failed\n");
		return 1;
	}
	return 0;
}

device_initcall(kcov_init);
