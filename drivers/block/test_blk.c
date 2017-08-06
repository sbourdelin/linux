/*
 * test_blk.c - A memory-based test block device driver.
 *
 * Copyright (c) 2017 Facebook, Inc.
 *
 * Parts derived from drivers/block/null_blk.c and drivers/block/brd.c,
 * copyright to respective owners.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/configfs.h>
#include <linux/radix-tree.h>

/*
 * Status flags for testb_device.
 *
 * CONFIGURED:	Device has been configured and turned on. Cannot reconfigure.
 * UP:		Device is currently on and visible in userspace.
 */
enum testb_device_flags {
	TESTB_DEV_FL_CONFIGURED	= 0,
	TESTB_DEV_FL_UP		= 1,
};

/*
 * testb_device represents the characteristics of a virtual device.
 *
 * @item:	The struct used by configfs to represent items in fs.
 * @lock:	Protect data of the device
 * @pages:	The storage of the device.
 * @flags:	TEST_DEV_FL_ flags to indicate various status.
 *
 * @power:	1 means on; 0 means off.
 * @size:	The size of the disk (in bytes).
 * @blocksize:	The block size for the request queue.
 * @nr_queues:	The number of queues.
 * @q_depth:	The depth of each queue.
 * @discard:	If enable discard
 */
struct testb_device {
	struct config_item item;
	spinlock_t lock;
	struct radix_tree_root pages;
	unsigned long flags;

	uint power;
	u64 size;
	uint blocksize;
	uint nr_queues;
	uint q_depth;
	uint discard;
};

static inline struct testb_device *to_testb_device(struct config_item *item)
{
	return item ? container_of(item, struct testb_device, item) : NULL;
}

static inline ssize_t testb_device_uint_attr_show(uint val, char *page)
{
	return snprintf(page, PAGE_SIZE, "%u\n", val);
}

static ssize_t
testb_device_uint_attr_store(uint *val, const char *page, size_t count)
{
	uint tmp;
	int result;

	result = kstrtouint(page, 0, &tmp);
	if (result)
		return result;

	*val = tmp;
	return count;
}

static inline ssize_t testb_device_u64_attr_show(u64 val, char *page)
{
	return snprintf(page, PAGE_SIZE, "%llu\n", val);
}

static ssize_t
testb_device_u64_attr_store(u64 *val, const char *page, size_t count)
{
	int result;
	u64 tmp;

	result = kstrtoull(page, 0, &tmp);
	if (result)
		return result;

	*val = tmp;
	return count;
}

/* The following macro should only be used with TYPE = {uint, u64}. */
#define TESTB_DEVICE_ATTR(NAME, TYPE)						\
static ssize_t									\
testb_device_##NAME##_show(struct config_item *item, char *page)		\
{										\
	return testb_device_##TYPE##_attr_show(					\
				to_testb_device(item)->NAME, page);		\
}										\
static ssize_t									\
testb_device_##NAME##_store(struct config_item *item, const char *page,		\
			    size_t count)					\
{										\
	if (test_bit(TESTB_DEV_FL_CONFIGURED, &to_testb_device(item)->flags))	\
		return -EBUSY;							\
	return testb_device_##TYPE##_attr_store(				\
			&to_testb_device(item)->NAME, page, count);		\
}										\
CONFIGFS_ATTR(testb_device_, NAME);

TESTB_DEVICE_ATTR(size, u64);
TESTB_DEVICE_ATTR(blocksize, uint);
TESTB_DEVICE_ATTR(nr_queues, uint);
TESTB_DEVICE_ATTR(q_depth, uint);
TESTB_DEVICE_ATTR(discard, uint);

static ssize_t testb_device_power_show(struct config_item *item, char *page)
{
	return testb_device_uint_attr_show(to_testb_device(item)->power, page);
}

static ssize_t testb_device_power_store(struct config_item *item,
				     const char *page, size_t count)
{
	struct testb_device *t_dev = to_testb_device(item);
	uint newp = 0;
	ssize_t ret;

	ret = testb_device_uint_attr_store(&newp, page, count);
	if (ret < 0)
		return ret;

	if (!t_dev->power && newp) {
		if (test_and_set_bit(TESTB_DEV_FL_UP, &t_dev->flags))
			return count;

		set_bit(TESTB_DEV_FL_CONFIGURED, &t_dev->flags);
		t_dev->power = newp;
	} else if (to_testb_device(item)->power && !newp) {
		t_dev->power = newp;
		clear_bit(TESTB_DEV_FL_UP, &t_dev->flags);
	}

	return count;
}

CONFIGFS_ATTR(testb_device_, power);

static struct configfs_attribute *testb_device_attrs[] = {
	&testb_device_attr_power,
	&testb_device_attr_size,
	&testb_device_attr_blocksize,
	&testb_device_attr_nr_queues,
	&testb_device_attr_q_depth,
	&testb_device_attr_discard,
	NULL,
};

static void testb_device_release(struct config_item *item)
{
	kfree(to_testb_device(item));
}

static struct configfs_item_operations testb_device_ops = {
	.release	= testb_device_release,
};

static struct config_item_type testb_device_type = {
	.ct_item_ops	= &testb_device_ops,
	.ct_attrs	= testb_device_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct
config_item *testb_group_make_item(struct config_group *group, const char *name)
{
	struct testb_device *t_dev;

	t_dev = kzalloc(sizeof(struct testb_device), GFP_KERNEL);
	if (!t_dev)
		return ERR_PTR(-ENOMEM);

	config_item_init_type_name(&t_dev->item, name, &testb_device_type);

	/* Initialize attributes with default values. */
	t_dev->size = 1024 * 1024 * 1024ULL;
	t_dev->blocksize = 512;
	t_dev->nr_queues = 2;
	t_dev->q_depth = 64;
	t_dev->discard = 1;

	return &t_dev->item;
}

static void
testb_group_drop_item(struct config_group *group, struct config_item *item)
{
	config_item_put(item);
}

static ssize_t memb_group_features_show(struct config_item *item, char *page)
{
	return snprintf(page, PAGE_SIZE, "\n");
}

CONFIGFS_ATTR_RO(memb_group_, features);

static struct configfs_attribute *testb_group_attrs[] = {
	&memb_group_attr_features,
	NULL,
};

static struct configfs_group_operations testb_group_ops = {
	.make_item	= testb_group_make_item,
	.drop_item	= testb_group_drop_item,
};

static struct config_item_type testb_group_type = {
	.ct_group_ops	= &testb_group_ops,
	.ct_attrs	= testb_group_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct configfs_subsystem testb_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "testb",
			.ci_type = &testb_group_type,
		},
	},
};

static int __init testb_init(void)
{
	int ret = 0;
	struct configfs_subsystem *subsys = &testb_subsys;

	config_group_init(&subsys->su_group);
	mutex_init(&subsys->su_mutex);

	ret = configfs_register_subsystem(subsys);
	return ret;
}

static void __exit testb_exit(void)
{
	configfs_unregister_subsystem(&testb_subsys);
}

module_init(testb_init);
module_exit(testb_exit);

MODULE_AUTHOR("Will Koh <kkc6196@fb.com>, Shaohua Li <shli@fb.com>");
MODULE_LICENSE("GPL");
