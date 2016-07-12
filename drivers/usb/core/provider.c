#include <linux/slab.h>
#include <linux/usb/provider.h>

static DEFINE_MUTEX(hcd_provider_mutex);
static LIST_HEAD(hcd_provider_list);

struct hcd_provider {
	struct device_node *np;
	struct usb_hcd *(*of_xlate)(struct of_phandle_args *args, void *data);
	void *data;
	struct list_head list;
};

struct hcd_provider *of_hcd_provider_register(struct device_node *np,
					      struct usb_hcd *(*of_xlate)(struct of_phandle_args *args, void *data),
					      void *data)
{
	struct hcd_provider *hcd_provider;

	if (!np)
		return ERR_PTR(-EINVAL);

	hcd_provider = kzalloc(sizeof(*hcd_provider), GFP_KERNEL);
	if (!hcd_provider)
		return ERR_PTR(-ENOMEM);

	hcd_provider->np = np;
	hcd_provider->of_xlate = of_xlate;
	hcd_provider->data = data;

	mutex_lock(&hcd_provider_mutex);
	list_add_tail(&hcd_provider->list, &hcd_provider_list);
	mutex_unlock(&hcd_provider_mutex);

	return hcd_provider;
}
EXPORT_SYMBOL_GPL(of_hcd_provider_register);

void of_hcd_provider_unregister(struct hcd_provider *hcd_provider)
{
	if (IS_ERR(hcd_provider))
		return;

	mutex_lock(&hcd_provider_mutex);
	list_del(&hcd_provider->list);
	mutex_unlock(&hcd_provider_mutex);

	kfree(hcd_provider);
}
EXPORT_SYMBOL_GPL(of_hcd_provider_unregister);

struct usb_hcd *of_hcd_xlate_simple(struct of_phandle_args *args, void *data)
{
	if (args->args_count != 0)
		return ERR_PTR(-EINVAL);
	return data;
}
EXPORT_SYMBOL_GPL(of_hcd_xlate_simple);

struct usb_hcd *of_hcd_get_from_provider(struct of_phandle_args *args)
{
	struct usb_hcd *hcd = ERR_PTR(-ENOENT);
	struct hcd_provider *provider;

	if (!args)
		return ERR_PTR(-EINVAL);

	mutex_lock(&hcd_provider_mutex);
	list_for_each_entry(provider, &hcd_provider_list, list) {
		if (provider->np == args->np) {
			hcd = provider->of_xlate(args, provider->data);
			break;
		}
	}
	mutex_unlock(&hcd_provider_mutex);

	return hcd;
}
EXPORT_SYMBOL_GPL(of_hcd_get_from_provider);
