/*
 * media-dev-allocator.c - Media Controller Device Allocator API
 *
 * Copyright (c) 2016 Shuah Khan <shuahkh@osg.samsung.com>
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *
 * This file is released under the GPLv2.
 * Credits: Suggested by Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 */

/*
 * This file adds a global refcounted Media Controller Device Instance API.
 * A system wide global media device list is managed and each media device
 * includes a kref count. The last put on the media device releases the media
 * device instance.
 *
*/

#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/usb.h>
#include <media/media-device.h>

static LIST_HEAD(media_device_list);
static DEFINE_MUTEX(media_device_lock);

struct media_device_instance {
	struct media_device mdev;
	struct list_head list;
	struct device *dev;
	struct kref refcount;
};

static inline struct media_device_instance *
to_media_device_instance(struct media_device *mdev)
{
	return container_of(mdev, struct media_device_instance, mdev);
}

static void media_device_instance_release(struct kref *kref)
{
	struct media_device_instance *mdi =
		container_of(kref, struct media_device_instance, refcount);

	dev_dbg(mdi->mdev.dev, "%s: mdev=%p\n", __func__, &mdi->mdev);

	mutex_lock(&media_device_lock);

	media_device_unregister(&mdi->mdev);
	media_device_cleanup(&mdi->mdev);

	list_del(&mdi->list);
	mutex_unlock(&media_device_lock);

	kfree(mdi);
}

static struct media_device *__media_device_get(struct device *dev,
					       bool allocate)
{
	struct media_device_instance *mdi;

	list_for_each_entry(mdi, &media_device_list, list) {
		if (mdi->dev == dev) {
			kref_get(&mdi->refcount);
			dev_dbg(dev, "%s: get mdev=%p\n",
				 __func__, &mdi->mdev);
			goto done;
		}
	}

	if (!allocate) {
		mdi = NULL;
		goto done;
	}

	mdi = kzalloc(sizeof(*mdi), GFP_KERNEL);
	if (!mdi)
		goto done;

	mdi->dev = dev;
	kref_init(&mdi->refcount);
	list_add_tail(&mdi->list, &media_device_list);

	dev_dbg(dev, "%s: alloc mdev=%p\n", __func__, &mdi->mdev);
done:
	return mdi ? &mdi->mdev : NULL;
}

struct media_device *media_device_usb_allocate(struct usb_device *udev,
					       char *driver_name)
{
	struct media_device *mdev;

	mutex_lock(&media_device_lock);
	mdev = __media_device_get(&udev->dev, true);
	if (!mdev) {
		mutex_unlock(&media_device_lock);
		return ERR_PTR(-ENOMEM);
	}

	/* check if media device is already initialized */
	if (!mdev->dev)
		__media_device_usb_init(mdev, udev, udev->product,
					driver_name);
	mutex_unlock(&media_device_lock);

	dev_dbg(&udev->dev, "%s\n", __func__);
	return mdev;
}
EXPORT_SYMBOL_GPL(media_device_usb_allocate);

void media_device_delete(struct media_device *mdev)
{
	struct media_device_instance *mdi = to_media_device_instance(mdev);

	dev_dbg(mdi->mdev.dev, "%s: mdev=%p\n", __func__, &mdi->mdev);
	kref_put(&mdi->refcount, media_device_instance_release);
}
EXPORT_SYMBOL_GPL(media_device_delete);
