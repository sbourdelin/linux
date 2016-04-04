#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/usb.h>
#include <linux/audit.h>
#include <linux/kdev_t.h>

static void log_string(struct audit_buffer *ab, char *key, char *val)
{
       if (val) {
               audit_log_format(ab, " %s=", key);
               audit_log_untrustedstring(ab, val);
       }
       else {
              audit_log_format(ab, " %s=%s", key, "?");
       } 

}

static void log_major_minor(struct audit_buffer *ab, struct device *dev)
{
       if (dev && dev->devt) {
               audit_log_format(ab, " major=%d", MAJOR(dev->devt));
               audit_log_format(ab, " minor=%d", MINOR(dev->devt));
       }
}

/* Blocking call when device has reference and will keep reference until
 * all notifiers are done, no usb_dev_get/ usb_dev_put required.
 */
static int audit_notify(struct notifier_block *self,
       unsigned long action, void *d)
{
       struct usb_device *usbdev = (struct usb_device *)d;
       char *op;
       struct audit_buffer *ab;

       switch (action) {
       case USB_DEVICE_ADD:
               op = "add";
               break;
       case USB_DEVICE_REMOVE:
               op =  "remove";
               break;
       default: /* ignore any other USB events */ 
	       return NOTIFY_DONE;
       }

       ab = audit_log_start(NULL, GFP_KERNEL, AUDIT_DEVICE_CHANGE);

       if (ab) {
               audit_log_format(ab, "action=%s", op);
               log_string(ab, "manufacturer", usbdev->manufacturer);
               log_string(ab, "product", usbdev->product);
               log_string(ab, "serial", usbdev->serial);
               log_major_minor(ab, &usbdev->dev);
               log_string(ab, "bus", "usb");
               audit_log_end(ab);
       }

       return NOTIFY_DONE;
}

static struct notifier_block audit_nb = {
       .notifier_call = audit_notify,
       .priority = INT_MIN
};

static int __init audit_device_init(void)
{
       pr_info("Registering usb audit notification callback\n");
       usb_register_notify(&audit_nb);
       return 0;
}

static void __exit audit_device_exit(void)
{
       pr_info("Unregistering usb audit notification callback\n");
       usb_unregister_notify(&audit_nb);
}

module_init(audit_device_init);
module_exit(audit_device_exit);

MODULE_LICENSE("GPL");
