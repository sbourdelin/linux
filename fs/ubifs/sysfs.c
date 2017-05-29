#include <linux/fs.h>

#include "ubifs.h"

static struct kset *ubifs_kset;

static void ubifs_sb_release(struct kobject *kobj)
{
	struct ubifs_info *c = container_of(kobj, struct ubifs_info, kobj);

	complete(&c->kobj_unregister);
}

static struct kobj_type ubifs_sb_ktype = {
	.sysfs_ops	= &kobj_sysfs_ops,
	.release	= ubifs_sb_release,
};

int ubifs_sysfs_register(struct ubifs_info *c)
{
	dev_t devt = c->vfs_sb->s_dev;
	int ret;

	c->kobj.kset = ubifs_kset;
	init_completion(&c->kobj_unregister);

	ret = kobject_init_and_add(&c->kobj, &ubifs_sb_ktype, NULL,
				   "%u:%u", MAJOR(devt), MINOR(devt));
	if (ret)
		goto out_put;

	ret = sysfs_create_link(&c->kobj, ubi_volume_kobj(c->ubi), "ubi");
	if (ret)
		goto out_del;

	return 0;

out_del:
	kobject_del(&c->kobj);
out_put:
	kobject_put(&c->kobj);
	wait_for_completion(&c->kobj_unregister);
	return ret;
}

void ubifs_sysfs_unregister(struct ubifs_info *c)
{
	sysfs_remove_link(&c->kobj, "ubi");
	kobject_del(&c->kobj);
	kobject_put(&c->kobj);
	wait_for_completion(&c->kobj_unregister);
}

int __init ubifs_sysfs_init(void)
{
	ubifs_kset = kset_create_and_add("ubifs", NULL, fs_kobj);
	if (!ubifs_kset)
		return -ENOMEM;

	return 0;
}

void ubifs_sysfs_exit(void)
{
	kset_unregister(ubifs_kset);
}
