#ifndef __USB_CORE_PROVIDER_H
#define __USB_CORE_PROVIDER_H

#include <linux/of.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>

struct hcd_provider;

#ifdef CONFIG_OF
struct hcd_provider *of_hcd_provider_register(struct device_node *np,
					      struct usb_hcd *(*of_xlate)(struct of_phandle_args *args, void *data),
					      void *data);
void of_hcd_provider_unregister(struct hcd_provider *hcd_provider);
struct usb_hcd *of_hcd_xlate_simple(struct of_phandle_args *args, void *data);
struct usb_hcd *of_hcd_get_from_provider(struct of_phandle_args *args);
#else
static inline
struct hcd_provider *of_hcd_provider_register(struct device_node *np,
					      struct usb_hcd *(*of_xlate)(struct of_phandle_args *args, void *data),
					      void *data)
{
	return ERR_PTR(-ENOSYS);
}
static inline void of_hcd_provider_unregister(struct hcd_provider *hcd_provider)
{
}
static inline struct usb_hcd *of_hcd_xlate_simple(struct of_phandle_args *args,
						  void *data)
{
	return ERR_PTR(-ENOSYS);
}
static inline struct usb_hcd *of_hcd_get_from_provider(struct of_phandle_args *args)
{
	return NULL;
}
#endif

#endif /* __USB_CORE_PROVIDER_H */
