/*
 * f_audio.c -- USB Audio class function driver
  *
 * Copyright (C) 2008 Bryan Wu <cooloney@kernel.org>
 * Copyright (C) 2008 Analog Devices, Inc
 *
 * Copyright (C) 2016 Ruslan Bilovol <ruslan.bilovol@gmail.com>
 *
 * Enter bugs at http://blackfin.uclinux.org/
 *
 * Licensed under the GPL-2 or later.
 */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/atomic.h>

#include "u_audio.h"
#include "u_uac1.h"

struct f_uac1 {
	struct gaudio gaudio;
	u8 ac_intf, as_out_intf;
	u8 ac_alt, as_out_alt;	/* needed for get_alt() */
};

static inline struct f_uac1 *func_to_uac1(struct usb_function *f)
{
	return container_of(f, struct f_uac1, gaudio.func);
}

/*
 * DESCRIPTORS ... most are static, but strings and full
 * configuration descriptors are built on demand.
 */

/*
 * We have two interfaces- AudioControl and AudioStreaming
 * TODO: only supcard playback currently
 */
#define F_AUDIO_AC_INTERFACE	0
#define F_AUDIO_AS_INTERFACE	1
#define F_AUDIO_NUM_INTERFACES	1

/* B.3.1  Standard AC Interface Descriptor */
static struct usb_interface_descriptor ac_interface_desc = {
	.bLength =		USB_DT_INTERFACE_SIZE,
	.bDescriptorType =	USB_DT_INTERFACE,
	.bNumEndpoints =	0,
	.bInterfaceClass =	USB_CLASS_AUDIO,
	.bInterfaceSubClass =	USB_SUBCLASS_AUDIOCONTROL,
};

/*
 * The number of AudioStreaming and MIDIStreaming interfaces
 * in the Audio Interface Collection
 */
DECLARE_UAC_AC_HEADER_DESCRIPTOR(1);

#define UAC_DT_AC_HEADER_LENGTH	UAC_DT_AC_HEADER_SIZE(F_AUDIO_NUM_INTERFACES)
/* 1 input terminal and 1 output terminal */
#define UAC_DT_TOTAL_LENGTH (UAC_DT_AC_HEADER_LENGTH + UAC_DT_INPUT_TERMINAL_SIZE \
	+ UAC_DT_OUTPUT_TERMINAL_SIZE)
/* B.3.2  Class-Specific AC Interface Descriptor */
static struct uac1_ac_header_descriptor_1 ac_header_desc = {
	.bLength =		UAC_DT_AC_HEADER_LENGTH,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubtype =	UAC_HEADER,
	.bcdADC =		__constant_cpu_to_le16(0x0100),
	.wTotalLength =		__constant_cpu_to_le16(UAC_DT_TOTAL_LENGTH),
	.bInCollection =	F_AUDIO_NUM_INTERFACES,
	.baInterfaceNr = {
	/* Interface number of the first AudioStream interface */
		[0] =		1,
	}
};

#define INPUT_TERMINAL_ID	1
static struct uac_input_terminal_descriptor input_terminal_desc = {
	.bLength =		UAC_DT_INPUT_TERMINAL_SIZE,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubtype =	UAC_INPUT_TERMINAL,
	.bTerminalID =		INPUT_TERMINAL_ID,
	.wTerminalType =	UAC_TERMINAL_STREAMING,
	.bAssocTerminal =	0,
	.wChannelConfig =	0x3,
};

#define OUTPUT_TERMINAL_ID	2
static struct uac1_output_terminal_descriptor output_terminal_desc = {
	.bLength		= UAC_DT_OUTPUT_TERMINAL_SIZE,
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubtype	= UAC_OUTPUT_TERMINAL,
	.bTerminalID		= OUTPUT_TERMINAL_ID,
	.wTerminalType		= UAC_OUTPUT_TERMINAL_SPEAKER,
	.bAssocTerminal		= 0,
	.bSourceID		= INPUT_TERMINAL_ID,
};

/* B.4.1  Standard AS Interface Descriptor */
static struct usb_interface_descriptor as_interface_alt_0_desc = {
	.bLength =		USB_DT_INTERFACE_SIZE,
	.bDescriptorType =	USB_DT_INTERFACE,
	.bAlternateSetting =	0,
	.bNumEndpoints =	0,
	.bInterfaceClass =	USB_CLASS_AUDIO,
	.bInterfaceSubClass =	USB_SUBCLASS_AUDIOSTREAMING,
};

static struct usb_interface_descriptor as_interface_alt_1_desc = {
	.bLength =		USB_DT_INTERFACE_SIZE,
	.bDescriptorType =	USB_DT_INTERFACE,
	.bAlternateSetting =	1,
	.bNumEndpoints =	1,
	.bInterfaceClass =	USB_CLASS_AUDIO,
	.bInterfaceSubClass =	USB_SUBCLASS_AUDIOSTREAMING,
};

/* B.4.2  Class-Specific AS Interface Descriptor */
static struct uac1_as_header_descriptor as_header_desc = {
	.bLength =		UAC_DT_AS_HEADER_SIZE,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubtype =	UAC_AS_GENERAL,
	.bTerminalLink =	INPUT_TERMINAL_ID,
	.bDelay =		1,
	.wFormatTag =		UAC_FORMAT_TYPE_I_PCM,
};

DECLARE_UAC_FORMAT_TYPE_I_DISCRETE_DESC(1);

static struct uac_format_type_i_discrete_descriptor_1 as_type_i_desc = {
	.bLength =		UAC_FORMAT_TYPE_I_DISCRETE_DESC_SIZE(1),
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubtype =	UAC_FORMAT_TYPE,
	.bFormatType =		UAC_FORMAT_TYPE_I,
	.bSubframeSize =	2,
	.bBitResolution =	16,
	.bSamFreqType =		1,
};

/* Standard ISO OUT Endpoint Descriptor */
static struct usb_endpoint_descriptor as_out_ep_desc  = {
	.bLength =		USB_DT_ENDPOINT_AUDIO_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_SYNC_ADAPTIVE
				| USB_ENDPOINT_XFER_ISOC,
	.wMaxPacketSize	=	cpu_to_le16(UAC1_OUT_EP_MAX_PACKET_SIZE),
	.bInterval =		4,
};

/* Class-specific AS ISO OUT Endpoint Descriptor */
static struct uac_iso_endpoint_descriptor as_iso_out_desc = {
	.bLength =		UAC_ISO_ENDPOINT_DESC_SIZE,
	.bDescriptorType =	USB_DT_CS_ENDPOINT,
	.bDescriptorSubtype =	UAC_EP_GENERAL,
	.bmAttributes = 	1,
	.bLockDelayUnits =	1,
	.wLockDelay =		__constant_cpu_to_le16(1),
};

static struct usb_descriptor_header *f_audio_desc[] = {
	(struct usb_descriptor_header *)&ac_interface_desc,
	(struct usb_descriptor_header *)&ac_header_desc,

	(struct usb_descriptor_header *)&input_terminal_desc,
	(struct usb_descriptor_header *)&output_terminal_desc,

	(struct usb_descriptor_header *)&as_interface_alt_0_desc,
	(struct usb_descriptor_header *)&as_interface_alt_1_desc,
	(struct usb_descriptor_header *)&as_header_desc,

	(struct usb_descriptor_header *)&as_type_i_desc,

	(struct usb_descriptor_header *)&as_out_ep_desc,
	(struct usb_descriptor_header *)&as_iso_out_desc,
	NULL,
};

enum {
	STR_AC_IF,
	STR_INPUT_TERMINAL,
	STR_INPUT_TERMINAL_CH_NAMES,
	STR_OUTPUT_TERMINAL,
	STR_AS_IF_ALT0,
	STR_AS_IF_ALT1,
};

static struct usb_string strings_uac1[] = {
	[STR_AC_IF].s = "AC Interface",
	[STR_INPUT_TERMINAL].s = "Input terminal",
	[STR_INPUT_TERMINAL_CH_NAMES].s = "Channels",
	[STR_OUTPUT_TERMINAL].s = "Output terminal",
	[STR_AS_IF_ALT0].s = "AS Interface",
	[STR_AS_IF_ALT1].s = "AS Interface",
	{ },
};

static struct usb_gadget_strings str_uac1 = {
	.language = 0x0409,	/* en-us */
	.strings = strings_uac1,
};

static struct usb_gadget_strings *uac1_strings[] = {
	&str_uac1,
	NULL,
};

/*
 * This function is an ALSA sound card following USB Audio Class Spec 1.0.
 */

static int audio_set_endpoint_req(struct usb_function *f,
		const struct usb_ctrlrequest *ctrl)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	int			value = -EOPNOTSUPP;
	u16			ep = le16_to_cpu(ctrl->wIndex);
	u16			len = le16_to_cpu(ctrl->wLength);
	u16			w_value = le16_to_cpu(ctrl->wValue);

	DBG(cdev, "bRequest 0x%x, w_value 0x%04x, len %d, endpoint %d\n",
			ctrl->bRequest, w_value, len, ep);

	switch (ctrl->bRequest) {
	case UAC_SET_CUR:
		value = len;
		break;

	case UAC_SET_MIN:
		break;

	case UAC_SET_MAX:
		break;

	case UAC_SET_RES:
		break;

	case UAC_SET_MEM:
		break;

	default:
		break;
	}

	return value;
}

static int audio_get_endpoint_req(struct usb_function *f,
		const struct usb_ctrlrequest *ctrl)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	int value = -EOPNOTSUPP;
	u8 ep = ((le16_to_cpu(ctrl->wIndex) >> 8) & 0xFF);
	u16 len = le16_to_cpu(ctrl->wLength);
	u16 w_value = le16_to_cpu(ctrl->wValue);

	DBG(cdev, "bRequest 0x%x, w_value 0x%04x, len %d, endpoint %d\n",
			ctrl->bRequest, w_value, len, ep);

	switch (ctrl->bRequest) {
	case UAC_GET_CUR:
	case UAC_GET_MIN:
	case UAC_GET_MAX:
	case UAC_GET_RES:
		value = len;
		break;
	case UAC_GET_MEM:
		break;
	default:
		break;
	}

	return value;
}

static int
f_audio_setup(struct usb_function *f, const struct usb_ctrlrequest *ctrl)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_request	*req = cdev->req;
	int			value = -EOPNOTSUPP;
	u16			w_index = le16_to_cpu(ctrl->wIndex);
	u16			w_value = le16_to_cpu(ctrl->wValue);
	u16			w_length = le16_to_cpu(ctrl->wLength);

	/* composite driver infrastructure handles everything; interface
	 * activation uses set_alt().
	 */
	switch (ctrl->bRequestType) {
	case USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_ENDPOINT:
		value = audio_set_endpoint_req(f, ctrl);
		break;

	case USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT:
		value = audio_get_endpoint_req(f, ctrl);
		break;

	default:
		ERROR(cdev, "invalid control req%02x.%02x v%04x i%04x l%d\n",
			ctrl->bRequestType, ctrl->bRequest,
			w_value, w_index, w_length);
	}

	/* respond with data transfer or status phase? */
	if (value >= 0) {
		DBG(cdev, "audio req%02x.%02x v%04x i%04x l%d\n",
			ctrl->bRequestType, ctrl->bRequest,
			w_value, w_index, w_length);
		req->zero = 0;
		req->length = value;
		value = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
		if (value < 0)
			ERROR(cdev, "audio response on err %d\n", value);
	}

	/* device either stalls (value < 0) or reports success */
	return value;
}

static int f_audio_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_gadget *gadget = cdev->gadget;
	struct device *dev = &gadget->dev;
	struct f_uac1 *uac1 = func_to_uac1(f);
	int ret = 0;

	/* No i/f has more than 2 alt settings */
	if (alt > 1) {
		dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
		return -EINVAL;
	}

	if (intf == uac1->ac_intf) {
		/* Control I/f has only 1 AltSetting - 0 */
		if (alt) {
			dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
			return -EINVAL;
		}
		return 0;
	}

	if (intf == uac1->as_out_intf) {
		uac1->as_out_alt = alt;

		if (alt)
			ret = gaudio_start_capture(&uac1->gaudio);
		else
			gaudio_stop_capture(&uac1->gaudio);
	} else {
		dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
		return -EINVAL;
	}

	return ret;
}

static int f_audio_get_alt(struct usb_function *f, unsigned intf)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_gadget *gadget = cdev->gadget;
	struct device *dev = &gadget->dev;
	struct f_uac1 *uac1 = func_to_uac1(f);

	if (intf == uac1->ac_intf)
		return uac1->ac_alt;
	else if (intf == uac1->as_out_intf)
		return uac1->as_out_alt;
	else
		dev_err(dev, "%s:%d Invalid Interface %d!\n",
			__func__, __LINE__, intf);

	return -EINVAL;
}


static void f_audio_disable(struct usb_function *f)
{
	struct f_uac1 *uac1 = func_to_uac1(f);

	uac1->as_out_alt = 0;

	gaudio_stop_capture(&uac1->gaudio);
}

/*-------------------------------------------------------------------------*/

/* audio function driver setup/binding */
static int f_audio_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_composite_dev *cdev = c->cdev;
	struct usb_gadget	*gadget = cdev->gadget;
	struct f_uac1		*uac1 = func_to_uac1(f);
	struct gaudio		*audio = func_to_gaudio(f);
	struct usb_string	*us;
	int			status;
	struct usb_ep		*ep = NULL;
	struct f_uac1_opts	*audio_opts;
	u8 *sam_freq;
	int rate;

	audio_opts = container_of(f->fi, struct f_uac1_opts, func_inst);

	us = usb_gstrings_attach(cdev, uac1_strings, ARRAY_SIZE(strings_uac1));
	if (IS_ERR(us))
		return PTR_ERR(us);
	ac_interface_desc.iInterface = us[STR_AC_IF].id;
	input_terminal_desc.iTerminal = us[STR_INPUT_TERMINAL].id;
	input_terminal_desc.iChannelNames = us[STR_INPUT_TERMINAL_CH_NAMES].id;
	output_terminal_desc.iTerminal = us[STR_OUTPUT_TERMINAL].id;
	as_interface_alt_0_desc.iInterface = us[STR_AS_IF_ALT0].id;
	as_interface_alt_1_desc.iInterface = us[STR_AS_IF_ALT1].id;

	/* Set channel numbers */
	input_terminal_desc.bNrChannels = num_channels(audio_opts->c_chmask);
	input_terminal_desc.wChannelConfig = cpu_to_le16(audio_opts->c_chmask);
	as_type_i_desc.bNrChannels = num_channels(audio_opts->c_chmask);
	as_type_i_desc.bSubframeSize = audio_opts->c_ssize;
	as_type_i_desc.bBitResolution = audio_opts->c_ssize * 8;

	/* Set sample rates */
	rate = audio_opts->c_srate;
	sam_freq = as_type_i_desc.tSamFreq[0];
	memcpy(sam_freq, &rate, 3);

	/* allocate instance-specific interface IDs, and patch descriptors */
	status = usb_interface_id(c, f);
	if (status < 0)
		goto fail;
	ac_interface_desc.bInterfaceNumber = status;
	uac1->ac_intf = status;
	uac1->ac_alt = 0;

	status = usb_interface_id(c, f);
	if (status < 0)
		goto fail;
	as_interface_alt_0_desc.bInterfaceNumber = status;
	as_interface_alt_1_desc.bInterfaceNumber = status;
	uac1->as_out_intf = status;
	uac1->as_out_alt = 0;

	audio->gadget = gadget;

	status = -ENODEV;

	/* allocate instance-specific endpoints */
	ep = usb_ep_autoconfig(cdev->gadget, &as_out_ep_desc);
	if (!ep)
		goto fail;
	audio->out_ep = ep;
	audio->out_ep->desc = &as_out_ep_desc;

	/* copy descriptors, and track endpoint copies */
	status = usb_assign_descriptors(f, f_audio_desc, f_audio_desc, NULL,
					NULL);
	if (status)
		goto fail;

	audio->out_ep_maxpsize = as_out_ep_desc.wMaxPacketSize;
	audio->params.c_chmask = audio_opts->c_chmask;
	audio->params.c_srate = audio_opts->c_srate;
	audio->params.c_ssize = audio_opts->c_ssize;

	status = gaudio_setup(audio, "UAC1_PCM", "UAC1_Gadget");
	if (status)
		goto err_card_register;

	return 0;

err_card_register:
	usb_free_all_descriptors(f);
fail:
	return status;
}

/*-------------------------------------------------------------------------*/

static inline struct f_uac1_opts *to_f_uac1_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct f_uac1_opts,
			    func_inst.group);
}

static void f_uac1_attr_release(struct config_item *item)
{
	struct f_uac1_opts *opts = to_f_uac1_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations f_uac1_item_ops = {
	.release	= f_uac1_attr_release,
};

#define UAC1_ATTRIBUTE(name)					\
static ssize_t f_uac1_opts_##name##_show(struct config_item *item,	\
					 char *page)			\
{									\
	struct f_uac1_opts *opts = to_f_uac1_opts(item);		\
	int result;							\
									\
	mutex_lock(&opts->lock);					\
	result = sprintf(page, "%u\n", opts->name);			\
	mutex_unlock(&opts->lock);					\
									\
	return result;							\
}									\
									\
static ssize_t f_uac1_opts_##name##_store(struct config_item *item,		\
					  const char *page, size_t len)	\
{									\
	struct f_uac1_opts *opts = to_f_uac1_opts(item);		\
	int ret;							\
	u32 num;							\
									\
	mutex_lock(&opts->lock);					\
	if (opts->refcnt) {						\
		ret = -EBUSY;						\
		goto end;						\
	}								\
									\
	ret = kstrtou32(page, 0, &num);					\
	if (ret)							\
		goto end;						\
									\
	opts->name = num;						\
	ret = len;							\
									\
end:									\
	mutex_unlock(&opts->lock);					\
	return ret;							\
}									\
									\
CONFIGFS_ATTR(f_uac1_opts_, name)

UAC1_ATTRIBUTE(c_chmask);
UAC1_ATTRIBUTE(c_srate);
UAC1_ATTRIBUTE(c_ssize);

static struct configfs_attribute *f_uac1_attrs[] = {
	&f_uac1_opts_attr_c_chmask,
	&f_uac1_opts_attr_c_srate,
	&f_uac1_opts_attr_c_ssize,
	NULL,
};

static struct config_item_type f_uac1_func_type = {
	.ct_item_ops	= &f_uac1_item_ops,
	.ct_attrs	= f_uac1_attrs,
	.ct_owner	= THIS_MODULE,
};

static void f_audio_free_inst(struct usb_function_instance *f)
{
	struct f_uac1_opts *opts;

	opts = container_of(f, struct f_uac1_opts, func_inst);
	kfree(opts);
}

static struct usb_function_instance *f_audio_alloc_inst(void)
{
	struct f_uac1_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	mutex_init(&opts->lock);
	opts->func_inst.free_func_inst = f_audio_free_inst;

	config_group_init_type_name(&opts->func_inst.group, "",
				    &f_uac1_func_type);

	opts->c_chmask = UAC1_DEF_CCHMASK;
	opts->c_srate = UAC1_DEF_CSRATE;
	opts->c_ssize = UAC1_DEF_CSSIZE;
	return &opts->func_inst;
}

static void f_audio_free(struct usb_function *f)
{
	struct gaudio *audio;
	struct f_uac1_opts *opts;

	audio = func_to_gaudio(f);
	opts = container_of(f->fi, struct f_uac1_opts, func_inst);
	kfree(audio);
	mutex_lock(&opts->lock);
	--opts->refcnt;
	mutex_unlock(&opts->lock);
}

static void f_audio_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct gaudio *audio = func_to_gaudio(f);

	gaudio_cleanup(audio);
	usb_free_all_descriptors(f);

	audio->gadget = NULL;
}

static struct usb_function *f_audio_alloc(struct usb_function_instance *fi)
{
	struct f_uac1 *uac1;
	struct f_uac1_opts *opts;

	/* allocate and initialize one new instance */
	uac1 = kzalloc(sizeof(*uac1), GFP_KERNEL);
	if (!uac1)
		return ERR_PTR(-ENOMEM);

	opts = container_of(fi, struct f_uac1_opts, func_inst);
	mutex_lock(&opts->lock);
	++opts->refcnt;
	mutex_unlock(&opts->lock);

	uac1->gaudio.func.name = "g_audio";
	uac1->gaudio.func.bind = f_audio_bind;
	uac1->gaudio.func.unbind = f_audio_unbind;
	uac1->gaudio.func.set_alt = f_audio_set_alt;
	uac1->gaudio.func.get_alt = f_audio_get_alt;
	uac1->gaudio.func.setup = f_audio_setup;
	uac1->gaudio.func.disable = f_audio_disable;
	uac1->gaudio.func.free_func = f_audio_free;

	return &uac1->gaudio.func;
}

DECLARE_USB_FUNCTION_INIT(uac1, f_audio_alloc_inst, f_audio_alloc);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bryan Wu");
MODULE_AUTHOR("Ruslan Bilovol");
