#ifndef __USB_TYPEC_ALTMODE_H
#define __USB_TYPEC_ALTMODE_H

#include <linux/device.h>

struct typec_altmode;

/**
 * struct typec_altmode_ops - Alternate mode specific operations vector
 * @vdm: Process VDM
 * @notify: Communication channel for platform and the alternate mode
 */
struct typec_altmode_ops {
	void (*vdm)(struct typec_altmode *altmode, u32 hdr, u32 *vdo, int cnt);
	int (*notify)(struct typec_altmode *altmode,
		      unsigned long conf, void *data);
};

int typec_altmode_notify(struct typec_altmode *altmode,
			 unsigned long conf, void *data);
int typec_altmode_send_vdm(struct typec_altmode *altmode,
			   u32 header, u32 *vdo, int count);
void typec_altmode_set_drvdata(struct typec_altmode *altmode, void *data);
void *typec_altmode_get_drvdata(struct typec_altmode *altmode);

void typec_altmode_register_ops(struct typec_altmode *altmode,
				struct typec_altmode_ops *ops);
struct typec_altmode *typec_altmode_get_plug(struct typec_altmode *altmode,
					     int index);
void typec_altmode_put_plug(struct typec_altmode *plug);

struct typec_altmode *typec_find_altmode(struct typec_altmode **altmodes,
					 size_t n, u16 svid);

/**
 * struct typec_altmode_driver - USB Type-C alternate mode device driver
 * @svid: Standard or Vendor ID of the alternate mode
 * @probe: Callback for device binding
 * @remove: Callback for device unbinding
 * @driver: Device driver model driver
 *
 * These drivers will be bind to the partner alternate mode devices. They will
 * handle all SVID specific communication using VDMs (Vendor Defined Messages).
 */
struct typec_altmode_driver {
	const u16 svid;
	int (*probe)(struct typec_altmode *altmode);
	void (*remove)(struct typec_altmode *altmode);
	struct device_driver driver;
};

#define to_altmode_driver(d) container_of(d, struct typec_altmode_driver, \
					  driver)

#define typec_altmode_register_driver(drv) \
		__typec_altmode_register_driver(drv, THIS_MODULE)
int __typec_altmode_register_driver(struct typec_altmode_driver *drv,
				    struct module *module);
void typec_altmode_unregister_driver(struct typec_altmode_driver *drv);

#define module_typec_altmode_driver(__typec_altmode_driver) \
	module_driver(__typec_altmode_driver, typec_altmode_register_driver, \
		      typec_altmode_unregister_driver)

#endif /* __USB_TYPEC_ALTMODE_H */
