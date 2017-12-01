/*
 * Valid access checker
 *
 * Copyright (c) 2016-2017 Joonsoo Kim <iamjoonsoo.kim@lge.com>
 */


#include <linux/kernel.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/kasan.h>
#include <linux/uaccess.h>

#include "vchecker.h"
#include "../slab.h"
#include "kasan.h"

struct vchecker {
	bool enabled;
	struct list_head cb_list;
};

enum vchecker_type_num {
	VCHECKER_TYPE_VALUE = 0,
	VCHECKER_TYPE_MAX,
};

struct vchecker_data {
	void *dummy;
};

struct vchecker_type {
	char *name;
	const struct file_operations *fops;
	int (*init)(struct kmem_cache *s, struct vchecker_cb *cb,
			char *buf, size_t cnt);
	void (*fini)(struct vchecker_cb *cb);
	void (*show)(struct kmem_cache *s, struct seq_file *f,
			struct vchecker_cb *cb, void *object);
	bool (*check)(struct kmem_cache *s, struct vchecker_cb *cb,
			void *object, bool write,
			unsigned long begin, unsigned long end);
};

struct vchecker_cb {
	unsigned long begin;
	unsigned long end;
	void *arg;
	struct vchecker_type *type;
	struct list_head list;
};

struct vchecker_value_arg {
	u64 mask;
	u64 value;
};

static struct dentry *debugfs_root;
static struct vchecker_type vchecker_types[VCHECKER_TYPE_MAX];
static DEFINE_MUTEX(vchecker_meta);
static DEFINE_SPINLOCK(report_lock);

static bool need_check(struct vchecker_cb *cb,
		unsigned long begin, unsigned long end)
{
	if (cb->end <= begin)
		return false;

	if (cb->begin >= end)
		return false;

	return true;
}

static void show_cb(struct kmem_cache *s, struct seq_file *f,
			struct vchecker_cb *cb, void *object)
{
	if (f) {
		seq_printf(f, "%s checker for offset %ld ~ %ld\n",
			cb->type->name, cb->begin, cb->end);
	} else {
		pr_err("%s checker for offset %ld ~ %ld at %p\n",
			cb->type->name, cb->begin, cb->end, object);
	}

	cb->type->show(s, f, cb, object);
}

static void add_cb(struct kmem_cache *s, struct vchecker_cb *cb)
{
	list_add_tail(&cb->list, &s->vchecker_cache.checker->cb_list);
}

static int remove_cbs(struct kmem_cache *s, struct vchecker_type *t)
{
	struct vchecker *checker = s->vchecker_cache.checker;
	struct vchecker_cb *cb, *tmp;

	list_for_each_entry_safe(cb, tmp, &checker->cb_list, list) {
		if (cb->type == t) {
			list_del(&cb->list);
			t->fini(cb);
			kfree(cb);
		}
	}

	return 0;
}

void vchecker_init_slab_obj(struct kmem_cache *s, const void *object)
{
	struct vchecker_data *data;

	data = (void *)object + s->vchecker_cache.data_offset;
	__memset(data, 0, sizeof(*data));
}

void vchecker_cache_create(struct kmem_cache *s,
			size_t *size, slab_flags_t *flags)
{
	*flags |= SLAB_VCHECKER;

	s->vchecker_cache.data_offset = *size;
	*size += sizeof(struct vchecker_data);
}

void vchecker_kmalloc(struct kmem_cache *s, const void *object, size_t size)
{
	struct vchecker *checker;
	struct vchecker_cb *cb;

	rcu_read_lock();
	checker = s->vchecker_cache.checker;
	if (!checker || !checker->enabled) {
		rcu_read_unlock();
		return;
	}

	list_for_each_entry(cb, &checker->cb_list, list) {
		kasan_poison_shadow(object + cb->begin,
				    round_up(cb->end - cb->begin,
					     KASAN_SHADOW_SCALE_SIZE),
				    KASAN_VCHECKER_GRAYZONE);
	}
	rcu_read_unlock();
}

void vchecker_enable_obj(struct kmem_cache *s, const void *object,
			size_t size, bool enable)
{
	struct vchecker *checker;
	struct vchecker_cb *cb;
	s8 shadow_val = READ_ONCE(*(s8 *)kasan_mem_to_shadow(object));
	s8 mark = enable ? KASAN_VCHECKER_GRAYZONE : 0;

	/* It would be freed object. We don't need to mark it */
	if (shadow_val < 0 && (u8)shadow_val != KASAN_VCHECKER_GRAYZONE)
		return;

	checker = s->vchecker_cache.checker;
	list_for_each_entry(cb, &checker->cb_list, list) {
		kasan_poison_shadow(object + cb->begin,
				round_up(cb->end - cb->begin,
				     KASAN_SHADOW_SCALE_SIZE), mark);
	}
}

static void vchecker_report(unsigned long addr, size_t size, bool write,
			unsigned long ret_ip, struct kmem_cache *s,
			struct vchecker_cb *cb, void *object)
{
	unsigned long flags;
	const char *bug_type = "invalid access";

	kasan_disable_current();
	spin_lock_irqsave(&report_lock, flags);
	pr_err("==================================================================\n");
	pr_err("BUG: VCHECKER: %s in %pS at addr %p\n",
		bug_type, (void *)ret_ip, (void *)addr);
	pr_err("%s of size %zu by task %s/%d\n",
		write ? "Write" : "Read", size,
		current->comm, task_pid_nr(current));
	show_cb(s, NULL, cb, object);

	describe_object(s, object, (const void *)addr);
	pr_err("==================================================================\n");
	add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);
	spin_unlock_irqrestore(&report_lock, flags);
	if (panic_on_warn)
		panic("panic_on_warn set ...\n");
	kasan_enable_current();
}

static bool vchecker_poisoned(void *addr, size_t size)
{
	s8 shadow_val;
	s8 *shadow_addr = kasan_mem_to_shadow(addr);
	size_t shadow_size = kasan_mem_to_shadow(addr + size - 1) -
				(void *)shadow_addr + 1;

	while (shadow_size) {
		shadow_val = *shadow_addr;
		shadow_size--;
		shadow_addr++;

		if (shadow_val == 0)
			continue;

		if (shadow_val == (s8)KASAN_VCHECKER_GRAYZONE)
			continue;

		if (shadow_val < 0)
			return false;

		if (shadow_size)
			return false;

		/* last byte check */
		if ((((unsigned long)addr + size - 1) & KASAN_SHADOW_MASK) >=
			shadow_val)
			return false;
	}

	return true;
}

bool vchecker_check(unsigned long addr, size_t size,
			bool write, unsigned long ret_ip)
{
	struct page *page;
	struct kmem_cache *s;
	void *object;
	struct vchecker *checker;
	struct vchecker_cb *cb;
	unsigned long begin, end;
	bool checked = false;

	if (current->kasan_depth)
		return false;

	page = virt_to_head_page((void *)addr);
	if (!PageSlab(page))
		return false;

	s = page->slab_cache;
	object = nearest_obj(s, page, (void *)addr);
	begin = addr - (unsigned long)object;
	end = begin + size;

	rcu_read_lock();
	checker = s->vchecker_cache.checker;
	if (!checker->enabled) {
		rcu_read_unlock();
		goto check_shadow;
	}

	list_for_each_entry(cb, &checker->cb_list, list) {
		if (!need_check(cb, begin, end))
			continue;

		checked = true;
		if (cb->type->check(s, cb, object, write, begin, end))
			continue;

		vchecker_report(addr, size, write, ret_ip, s, cb, object);
		rcu_read_unlock();
		return true;
	}
	rcu_read_unlock();

	if (checked)
		return true;

check_shadow:
	return vchecker_poisoned((void *)addr, size);
}

static ssize_t vchecker_type_write(struct file *filp, const char __user *ubuf,
			size_t cnt, loff_t *ppos,
			enum vchecker_type_num type)
{
	char *buf;
	struct kmem_cache *s = file_inode(filp)->i_private;
	struct vchecker_type *t = NULL;
	struct vchecker_cb *cb = NULL;
	bool remove = false;
	int ret = -EINVAL;

	if (cnt >= PAGE_SIZE)
		return -EINVAL;

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, ubuf, cnt)) {
		kfree(buf);
		return -EFAULT;
	}

	if (isspace(buf[0]))
		remove = true;
	buf[cnt - 1] = '\0';

	mutex_lock(&vchecker_meta);
	if (s->vchecker_cache.checker->enabled)
		goto err;

	t = &vchecker_types[type];

	if (remove) {
		remove_cbs(s, t);
		goto out;
	}

	cb = kzalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb) {
		ret = -ENOMEM;
		goto err;
	}

	cb->type = t;
	INIT_LIST_HEAD(&cb->list);

	ret = t->init(s, cb, buf, cnt);
	if (ret)
		goto err;

	add_cb(s, cb);

out:
	mutex_unlock(&vchecker_meta);
	kfree(buf);

	return cnt;

err:
	mutex_unlock(&vchecker_meta);
	kfree(buf);
	kfree(cb);

	return ret;
}

static int vchecker_type_show(struct seq_file *f, enum vchecker_type_num type)
{
	struct kmem_cache *s = f->private;
	struct vchecker *checker;
	struct vchecker_cb *cb;

	mutex_lock(&vchecker_meta);
	checker = s->vchecker_cache.checker;
	list_for_each_entry(cb, &checker->cb_list, list) {
		if (cb->type != &vchecker_types[type])
			continue;

		show_cb(s, f, cb, NULL);
	}
	mutex_unlock(&vchecker_meta);

	return 0;
}

static int enable_show(struct seq_file *f, void *v)
{
	struct kmem_cache *s = f->private;
	struct vchecker *checker = s->vchecker_cache.checker;
	struct vchecker_cb *cb;

	mutex_lock(&vchecker_meta);

	seq_printf(f, "%s\n", checker->enabled ? "1" : "0");
	list_for_each_entry(cb, &checker->cb_list, list)
		show_cb(s, f, cb, NULL);

	mutex_unlock(&vchecker_meta);

	return 0;
}

static int enable_open(struct inode *inode, struct file *file)
{
	return single_open(file, enable_show, inode->i_private);
}

static ssize_t enable_write(struct file *filp, const char __user *ubuf,
			size_t cnt, loff_t *ppos)
{
	char enable_char;
	bool enable;
	struct kmem_cache *s = file_inode(filp)->i_private;

	if (cnt >= PAGE_SIZE || cnt == 0)
		return -EINVAL;

	if (copy_from_user(&enable_char, ubuf, 1))
		return -EFAULT;

	if (enable_char == '0')
		enable = false;
	else if (enable_char == '1')
		enable = true;
	else
		return -EINVAL;

	mutex_lock(&vchecker_meta);
	if (enable && list_empty(&s->vchecker_cache.checker->cb_list)) {
		mutex_unlock(&vchecker_meta);
		return -EINVAL;
	}
	s->vchecker_cache.checker->enabled = enable;

	/*
	 * After this operation, it is guaranteed that there is no user
	 * left that accesses checker's cb list if vchecker is disabled.
	 */
	synchronize_sched();
	vchecker_enable_cache(s, enable);
	mutex_unlock(&vchecker_meta);

	return cnt;
}

static const struct file_operations enable_fops = {
	.open		= enable_open,
	.write		= enable_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int init_value(struct kmem_cache *s, struct vchecker_cb *cb,
				char *buf, size_t cnt)
{
	unsigned long begin;
	u64 mask;
	u64 value;
	struct vchecker_value_arg *arg;
	unsigned long max_size = round_up(s->object_size, sizeof(u64));

	BUILD_BUG_ON(sizeof(u64) != KASAN_SHADOW_SCALE_SIZE);

	if (sscanf(buf, "%lu %llx %llu", &begin, &mask, &value) != 3)
		return -EINVAL;

	if (!IS_ALIGNED(begin, KASAN_SHADOW_SCALE_SIZE))
		return -EINVAL;

	if (begin > max_size - sizeof(value))
		return -EINVAL;

	arg = kzalloc(sizeof(struct vchecker_value_arg), GFP_KERNEL);
	if (!arg)
		return -ENOMEM;

	arg->mask = mask;
	arg->value = value;

	cb->begin = begin;
	cb->end = begin + sizeof(value);
	cb->arg = arg;

	return 0;
}

static void fini_value(struct vchecker_cb *cb)
{
	kfree(cb->arg);
}

static void show_value(struct kmem_cache *s, struct seq_file *f,
			struct vchecker_cb *cb, void *object)
{
	struct vchecker_value_arg *arg = cb->arg;

	if (f)
		seq_printf(f, "(mask 0x%llx value %llu) invalid value %llu\n\n",
			arg->mask, arg->value, arg->value & arg->mask);
	else
		pr_err("(mask 0x%llx value %llu) invalid value %llu\n\n",
			arg->mask, arg->value, arg->value & arg->mask);
}

static bool check_value(struct kmem_cache *s, struct vchecker_cb *cb,
			void *object, bool write,
			unsigned long begin, unsigned long end)
{
	struct vchecker_value_arg *arg;
	u64 value;

	arg = cb->arg;
	value = *(u64 *)(object + begin);
	if ((value & arg->mask) != (arg->value & arg->mask))
		return true;

	return false;
}

static int value_show(struct seq_file *f, void *v)
{
	return vchecker_type_show(f, VCHECKER_TYPE_VALUE);
}

static int value_open(struct inode *inode, struct file *file)
{
	return single_open(file, value_show, inode->i_private);
}

static ssize_t value_write(struct file *filp, const char __user *ubuf,
			size_t cnt, loff_t *ppos)
{
	return vchecker_type_write(filp, ubuf, cnt, ppos,
				VCHECKER_TYPE_VALUE);
}

static const struct file_operations fops_value = {
	.open		= value_open,
	.write		= value_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct vchecker_type vchecker_types[VCHECKER_TYPE_MAX] = {
	{ "value", &fops_value, init_value, fini_value,
		show_value, check_value },
};

static void free_vchecker(struct kmem_cache *s)
{
	int i;

	if (!s->vchecker_cache.checker)
		return;

	for (i = 0; i < ARRAY_SIZE(vchecker_types); i++)
		remove_cbs(s, &vchecker_types[i]);
	kfree(s->vchecker_cache.checker);
}

static void __fini_vchecker(struct kmem_cache *s)
{
	debugfs_remove_recursive(s->vchecker_cache.dir);
	free_vchecker(s);
}

void fini_vchecker(struct kmem_cache *s)
{
	mutex_lock(&vchecker_meta);
	__fini_vchecker(s);
	mutex_unlock(&vchecker_meta);
}

static int alloc_vchecker(struct kmem_cache *s)
{
	struct vchecker *checker;

	if (s->vchecker_cache.checker)
		return 0;

	checker = kzalloc(sizeof(*checker), GFP_KERNEL);
	if (!checker)
		return -ENOMEM;

	INIT_LIST_HEAD(&checker->cb_list);
	s->vchecker_cache.checker = checker;

	return 0;
}

static int register_debugfs(struct kmem_cache *s)
{
	int i;
	struct dentry *dir;
	struct vchecker_type *t;

	if (s->vchecker_cache.dir)
		return 0;

	dir = debugfs_create_dir(s->name, debugfs_root);
	if (!dir)
		return -ENOMEM;

	s->vchecker_cache.dir = dir;
	if (!debugfs_create_file("enable", 0600, dir, s, &enable_fops))
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(vchecker_types); i++) {
		t = &vchecker_types[i];
		if (!debugfs_create_file(t->name, 0600, dir, s, t->fops))
			return -ENOMEM;
	}

	return 0;
}

int init_vchecker(struct kmem_cache *s)
{
	if (!debugfs_root || !s->name)
		return 0;

	mutex_lock(&vchecker_meta);
	if (alloc_vchecker(s)) {
		mutex_unlock(&vchecker_meta);
		return -ENOMEM;
	}

	if (register_debugfs(s)) {
		__fini_vchecker(s);
		mutex_unlock(&vchecker_meta);
		return -ENOMEM;
	}
	mutex_unlock(&vchecker_meta);

	return 0;
}

static int __init vchecker_debugfs_init(void)
{
	debugfs_root = debugfs_create_dir("vchecker", NULL);
	if (!debugfs_root)
		return -ENOMEM;

	init_vcheckers();

	return 0;
}
core_initcall(vchecker_debugfs_init);
