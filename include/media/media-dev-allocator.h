/*
 * media-dev-allocator.h - Media Controller Device Allocator API
 *
 * Copyright (c) 2016 Shuah Khan <shuahkh@osg.samsung.com>
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *
 * This file is released under the GPLv2.
 * Credits: Suggested by Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 */

/*
 * This file adds a global ref-counted Media Controller Device Instance API.
 * A system wide global media device list is managed and each media device
 * includes a kref count. The last put on the media device releases the media
 * device instance.
*/

#ifndef _MEDIA_DEV_ALLOCTOR_H
#define _MEDIA_DEV_ALLOCTOR_H

struct usb_device;

#ifdef CONFIG_MEDIA_CONTROLLER
/**
 * DOC: Media Controller Device Allocator API
 *
 * When media device belongs to more than one driver, the shared media device
 * is allocated with the shared struct device as the key for look ups.
 *
 * Shared media device should stay in registered state until the last driver
 * unregisters it. In addition, media device should be released when all the
 * references are released. Each driver gets a reference to the media device
 * during probe, when it allocates the media device. If media device is already
 * allocated, allocate API bumps up the refcount and return the existing media
 * device. Driver puts the reference back from its disconnect routine when it
 * calls media_device_unregister_put().
 *
 * Media device is unregistered and cleaned up from the kref put handler to
 * ensure that the media device stays in registered state until the last driver
 * unregisters the media device.
 *
 * Media Device can be in any the following states:
 *
 * - Allocated
 * - Registered (could be tied to more than one driver)
 * - Unregistered and released in the kref put handler.
 *
 * Driver Usage:
 *
 * Drivers should use the media-core routines to get register reference and
 * use the media_device_unregister_put() routine to make sure the shared
 * media device delete is handled correctly.
 *
 * driver probe:
 *	Call media_device_usb_allocate() to allocate or get a reference
 *	Call media_device_register(), if media devnode isn't registered
 *
 * driver disconnect:
 *	Call media_device_unregister_put() to put the reference.
 *
 */

/**
 * media_device_usb_allocate() - Allocate and return media device
 *
 * @udev - struct usb_device pointer
 * @driver_name
 *
 * This interface should be called to allocate a media device when multiple
 * drivers share usb_device and the media device. This interface allocates
 * media device and calls media_device_usb_init() to initialize it.
 *
 */
struct media_device *media_device_usb_allocate(struct usb_device *udev,
					       char *driver_name);
/**
 * media_device_allocate() - Allocate and return global media device
 *
 * @dev - struct device pointer
 *
 * This interface should be called to allocate media device. A new media
 * device instance is created and added to the system wide media device
 * instance list. If media device instance exists, media_device_allocate()
 * increments the reference count and returns the media device. When more
 * than one driver control the media device, the first driver to probe will
 * allocate and the second driver when it calls  media_device_allocate(),
 * it will get a reference.
 *
 */
struct media_device *media_device_allocate(struct device *dev);
/**
 * media_device_get() -	Get reference to a registered media device.
 *
 * @mdev - struct media_device pointer
 *
 * This interface should be called to get a reference to an allocated media
 * device.
 */
struct media_device *media_device_get(struct media_device *mdev);
/**
 * media_device_put() - Release media device. Calls kref_put().
 *
 * @mdev - struct media_device pointer
 *
 * This interface should be called to put Media Device Instance kref.
 */
void media_device_put(struct media_device *mdev);
#else
static inline struct media_device *media_device_usb_allocate(
			struct usb_device *udev, char *driver_name)
			{ return NULL; }
static inline struct media_device *media_device_allocate(struct device *dev)
			{ return NULL; }
static inline struct media_device *media_device_get(struct media_device *mdev)
			{ return NULL; }
static inline void media_device_put(struct media_device *mdev) { }
#endif /* CONFIG_MEDIA_CONTROLLER */
#endif
