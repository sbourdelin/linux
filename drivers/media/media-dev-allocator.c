/*
 * media-devkref.c - Media Controller Device Allocator API
 *
 * Copyright (c) 2016 Shuah Khan <shuahkh@osg.samsung.com>
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *
 * This file is released under the GPLv2.
 * Credits: Suggested by Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 */

/*
 * This file adds Media Controller Device Instance with
 * Kref support. A system wide global media device list
 * is managed and each  media device is refcounted. The
 * last put on the media device releases the media device
 * instance.
*/

#ifdef CONFIG_MEDIA_CONTROLLER

#include <linux/slab.h>
#include <linux/kref.h>
#include <media/media-device.h>

static LIST_HEAD(media_device_list);
static LIST_HEAD(media_device_to_delete_list);
static DEFINE_MUTEX(media_device_lock);

struct media_device_instance {
	struct media_device mdev;
	struct list_head list;
	struct list_head to_delete_list;
	struct device *dev;
	struct kref refcount;
	bool to_delete; /* should be set when devnode is deleted */
};

static struct media_device *__media_device_get(struct device *dev,
					       bool alloc, bool kref)
{
	struct media_device_instance *mdi;

	mutex_lock(&media_device_lock);

	list_for_each_entry(mdi, &media_device_list, list) {
		if (mdi->dev == dev) {
			if (kref) {
				kref_get(&mdi->refcount);
				pr_info("%s: mdev=%p\n", __func__, &mdi->mdev);
			}
			goto done;
		}
	}

	if (!alloc) {
		mdi = NULL;
		goto done;
	}

	mdi = kzalloc(sizeof(*mdi), GFP_KERNEL);
	if (!mdi)
		goto done;

	mdi->dev = dev;
	kref_init(&mdi->refcount);
	list_add_tail(&mdi->list, &media_device_list);
	pr_info("%s: mdev=%p\n", __func__, &mdi->mdev);

done:
	mutex_unlock(&media_device_lock);

	return mdi ? &mdi->mdev : NULL;
}

struct media_device *media_device_get(struct device *dev)
{
	pr_info("%s\n", __func__);
	return __media_device_get(dev, true, true);
}
EXPORT_SYMBOL_GPL(media_device_get);

/* Don't increment kref - this is a search and find */
struct media_device *media_device_find(struct device *dev)
{
	pr_info("%s\n", __func__);
	return __media_device_get(dev, false, false);
}
EXPORT_SYMBOL_GPL(media_device_find);

/* don't allocate - increment kref if one is found */
struct media_device *media_device_get_ref(struct device *dev)
{
	pr_info("%s\n", __func__);
	return __media_device_get(dev, false, true);
}
EXPORT_SYMBOL_GPL(media_device_get_ref);

static void media_device_instance_release(struct kref *kref)
{
	struct media_device_instance *mdi =
		container_of(kref, struct media_device_instance, refcount);

	pr_info("%s: mdev=%p\n", __func__, &mdi->mdev);

	list_del(&mdi->list);
	kfree(mdi);
}

void media_device_put(struct device *dev)
{
	struct media_device_instance *mdi;

	mutex_lock(&media_device_lock);
	/* search first in the media_device_list */
	list_for_each_entry(mdi, &media_device_list, list) {
		if (mdi->dev == dev) {
			pr_info("%s: mdev=%p\n", __func__, &mdi->mdev);
			kref_put(&mdi->refcount, media_device_instance_release);
			goto done;
		}
	}
	/* search in the media_device_to_delete_list */
	list_for_each_entry(mdi, &media_device_to_delete_list, to_delete_list) {
		if (mdi->dev == dev) {
			pr_info("%s: mdev=%p\n", __func__, &mdi->mdev);
			kref_put(&mdi->refcount, media_device_instance_release);
			goto done;
		}
	}
done:
	mutex_unlock(&media_device_lock);
}
EXPORT_SYMBOL_GPL(media_device_put);

void media_device_set_to_delete_state(struct device *dev)
{
	struct media_device_instance *mdi;

	mutex_lock(&media_device_lock);
	list_for_each_entry(mdi, &media_device_list, list) {
		if (mdi->dev == dev) {
			pr_info("%s: mdev=%p\n", __func__, &mdi->mdev);
			mdi->to_delete = true;
			list_move(&mdi->list, &media_device_to_delete_list);
			goto done;
		}
	}
done:
	mutex_unlock(&media_device_lock);
}
EXPORT_SYMBOL_GPL(media_device_set_to_delete_state);

#endif /* CONFIG_MEDIA_CONTROLLER */
