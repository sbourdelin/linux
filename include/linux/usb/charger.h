#ifndef __LINUX_USB_CHARGER_H__
#define __LINUX_USB_CHARGER_H__

#include <uapi/linux/usb/ch9.h>
#include <uapi/linux/usb/charger.h>

#define CHARGER_NAME_MAX	30
#define work_to_charger(w)	(container_of((w), struct usb_charger, work))

/* Current range by charger type */
struct usb_charger_current {
	unsigned int sdp_min;
	unsigned int sdp_max;
	unsigned int dcp_min;
	unsigned int dcp_max;
	unsigned int cdp_min;
	unsigned int cdp_max;
	unsigned int aca_min;
	unsigned int aca_max;
};

struct usb_charger_nb {
	struct notifier_block	nb;
	struct usb_charger	*uchger;
};

/**
 * struct usb_charger - describe one usb charger
 * @name: Charger name.
 * @list: For linking usb charger instances into one global list.
 * @uchger_nh: Notifier head for users to register.
 * @lock: Protect the notifier head and charger.
 * @id: The charger id.
 * @type: The charger type, it can be SDP_TYPE, DCP_TYPE, CDP_TYPE or
 * ACA_TYPE.
 * @state: Indicate the charger state.
 * @cur: Describe the current range by charger type.
 * @work: Workqueue to be used for reporting users current has changed.
 * @extcon_dev: For supporting extcon usb gpio device to detect usb cable
 * event (plugin or not).
 * @extcon_nb: Report the events to charger from extcon device.
 * @extcon_type_nb: Report the charger type from extcon device.
 * @gadget: One usb charger is always tied to one usb gadget.
 * @old_gadget_state: Record the previous state of one usb gadget to check if
 * the gadget state is changed. If gadget state is changed, then notify the
 * event to charger.
 * @sdp_default_cur_change: Check if it is needed to change the SDP charger
 * default maximum current.
 * @charger_detect: User can detect the charger type by software or hardware,
 * then the charger detection method can be implemented if you need to detect
 * the charger type by software.
 *
 * Users can set and 'charger_detect' callbacks directly according to their
 * own requirements. Beyond that, users can not touch anything else directly
 * in this structure.
 */
struct usb_charger {
	char				name[CHARGER_NAME_MAX];
	struct list_head		list;
	struct raw_notifier_head	uchger_nh;
	struct mutex			lock;
	int				id;
	enum usb_charger_type		type;
	enum usb_charger_state		state;
	struct usb_charger_current	cur;
	struct work_struct		work;

	struct extcon_dev		*extcon_dev;
	struct usb_charger_nb		extcon_nb;
	struct usb_charger_nb		extcon_type_nb;

	struct usb_gadget		*gadget;
	enum usb_device_state		old_gadget_state;
	unsigned int			sdp_default_cur_change;

	enum usb_charger_type	(*charger_detect)(struct usb_charger *);
};

#ifdef CONFIG_USB_CHARGER
struct usb_charger *usb_charger_get_instance(void);

int usb_charger_register_notify(struct usb_charger *uchger,
				struct notifier_block *nb);
int usb_charger_unregister_notify(struct usb_charger *uchger,
				  struct notifier_block *nb);

int usb_charger_get_current(struct usb_charger *uchger,
			    unsigned int *min, unsigned int *max);

int usb_charger_set_cur_limit_by_type(struct usb_charger *uchger,
				      enum usb_charger_type type,
				      unsigned int cur_limit);
int usb_charger_set_cur_limit_by_gadget(struct usb_gadget *gadget,
					unsigned int cur_limit);

int usb_charger_plug_by_gadget(struct usb_gadget *gadget, unsigned long state);
enum usb_charger_type usb_charger_get_type(struct usb_charger *uchger);
int usb_charger_detect_type(struct usb_charger *uchger);
enum usb_charger_state usb_charger_get_state(struct usb_charger *uchger);

int usb_charger_init(struct usb_gadget *ugadget);
int usb_charger_exit(struct usb_gadget *ugadget);
#else
static inline struct usb_charger *usb_charger_get_instance(void)
{
	return ERR_PTR(-ENODEV);
}

static inline int usb_charger_register_notify(struct usb_charger *uchger,
					      struct notifier_block *nb)
{
	return 0;
}

static inline int usb_charger_unregister_notify(struct usb_charger *uchger,
						struct notifier_block *nb)
{
	return 0;
}

static inline int usb_charger_get_current(struct usb_charger *uchger,
					  unsigned int *min, unsigned int *max)
{
	return 0;
}

static inline int
usb_charger_set_cur_limit_by_type(struct usb_charger *uchger,
				  enum usb_charger_type type,
				  unsigned int cur_limit)
{
	return 0;
}

static inline int
usb_charger_set_cur_limit_by_gadget(struct usb_gadget *gadget,
				    unsigned int cur_limit)
{
	return 0;
}

static inline enum usb_charger_type
usb_charger_get_type(struct usb_charger *uchger)
{
	return UNKNOWN_TYPE;
}

static inline enum usb_charger_state
usb_charger_get_state(struct usb_charger *uchger)
{
	return USB_CHARGER_REMOVE;
}

static inline int usb_charger_detect_type(struct usb_charger *uchger)
{
	return 0;
}

static inline int usb_charger_plug_by_gadget(struct usb_gadget *gadget,
					     unsigned long state)
{
	return 0;
}

static inline int usb_charger_init(struct usb_gadget *ugadget)
{
	return 0;
}

static inline int usb_charger_exit(struct usb_gadget *ugadget)
{
	return 0;
}
#endif

#endif /* __LINUX_USB_CHARGER_H__ */
