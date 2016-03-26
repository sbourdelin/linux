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
 * This file adds Media Controller Device Instance with Kref support.
 * A system wide global media device list is managed and each media
 * device is refcounted. The last put on the media device releases
 * the media device instance.
*/

#ifndef _MEDIA_DEV_ALLOCTOR_H
#define _MEDIA_DEV_ALLOCTOR_H

#ifdef CONFIG_MEDIA_CONTROLLER
/**
 * DOC: Media Controller Device Allocator API
 * There are known problems with media device life time management. When media
 * device is released while an media ioctl is in progress, ioctls fail with
 * use-after-free errors and kernel hangs in some cases.
 * 
 * Media Device can be in any the following states:
 * 
 * - Allocated
 * - Registered (could be tied to more than one driver)
 * - Unregistered, not in use (media device file is not open)
 * - Unregistered, in use (media device file is not open)
 * - Released
 * 
 * When media device belongs to  more than one driver, registrations should be
 * refcounted to avoid unregistering when one of the drivers does unregister.
 * A refcount field in the struct media_device covers this case. Unregister on
 * a Media Allocator media device is a kref_put() call. The media device should
 * be unregistered only when the last unregister occurs.
 * 
 * When a media device is in use when it is unregistered, it should not be
 * released until the application exits when it detects the unregistered
 * status. Media device that is in use when it is unregistered is moved to
 * to_delete_list. When the last unregister occurs, media device is unregistered
 * and becomes an unregistered, still allocated device. Unregister marks the
 * device to be deleted.
 * 
 * When media device belongs to more than one driver, as both drivers could be
 * unbound/bound, driver should not end up getting stale media device that is
 * on its way out. Moving the unregistered media device to to_delete_list helps
 * this case as well.
 */
/**
 * media_device_get() - Allocate and return global media device
 *
 * @mdev
 *
 * This interface should be called to allocate media device. A new media
 * device instance is created and added to the system wide media device
 * instance list. If media device instance exists, media_device_get()
 * increments the reference count and returns the media device. When
 * more than one driver control the media device, the first driver to
 * probe will allocate and the second driver when it calls media_device_get()
 * it will get a reference.
 *
 */
struct media_device *media_device_get(struct device *dev);
/**
 * media_device_get_ref() - Get reference to an allocated and registered
 *			    media device.
 *
 * @mdev
 *
 * This interface should be called to get a reference to an allocated media
 * device. media_open() ioctl should call this to hold a reference to ensure
 * the media device will not be released until the media_release() does a put
 * on it.
 */
struct media_device *media_device_get_ref(struct device *dev);
/**
 * media_device_find() - Find an allocated and registered media device.
 *
 * @mdev
 *
 * This interface should be called to find a media device. This will not
 * incremnet the reference count.
 */
struct media_device *media_device_find(struct device *dev);
/**
 * media_device_put() - Release refcounted media device. Calls kref_put()
 *
 * @mdev
 *
 * This interface should be called to decrement refcount.
 */
void media_device_put(struct device *dev);
/**
 * media_device_set_to_delete_state() - Set the state to be deleted.
 *
 * @mdev
 *
 * This interface is used to not release the media device under from
 * an active ioctl if unregister happens.
 */
void media_device_set_to_delete_state(struct device *dev);
#else
struct media_device *media_device_get(struct device *dev) { return NULL; }
struct media_device *media_device_find(struct device *dev) { return NULL; }
void media_device_put(struct media_device *mdev) { }
void media_device_set_to_delete_state(struct device *dev) { }
#endif /* CONFIG_MEDIA_CONTROLLER */
#endif
