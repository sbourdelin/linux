// SPDX-License-Identifier: GPL-2.0+
/*
 * f_uac3.c -- USB Audio Class 3.0 Function
 *
 * Copyright (C) 2017 Ruslan Bilovol <ruslan.bilovol@gmail.com>
 */

#include <linux/usb/audio.h>
#include <linux/usb/audio-v2.h>
#include <linux/usb/audio-v3.h>
#include <linux/module.h>

#include "u_audio.h"
#include "u_uac3.h"

/*
 * The driver implements Generic I/O Profile (BAOF + BAIF)
 * from BasicAudioDevice3.0 spec:
 *    USB-OUT -> IT_1 -> FU_2 -> OT_3 -> ALSA Capture
 *    ALSA Playback -> IT_4 -> FU_5 -> OT_6 -> USB-IN
 *
 * Capture and Playback belong to independent Power Domains
 * PD_10 and PD_11 respectively.
 *
 * Capture and Playback sampling rates are independently
 * controlled by two clock sources:
 *    CLK_9 := c_srate, and CLK_12 := p_srate
 *
 * Entity IDs are taken from BasicAudioDevice3.0 spec.
 * The only difference is in additional playback clock
 * source which is required for independent sampling rate
 * of Capture and Playback channels.
 */

#define USB_OUT_IT_ID	1
#define USB_OUT_FU_ID	2
#define IO_OUT_OT_ID	3
#define IO_IN_IT_ID	4
#define USB_IN_FU_ID	5
#define USB_IN_OT_ID	6

#define USB_OUT_CLK_ID	9
#define USB_IN_CLK_ID	12

#define USB_OUT_PD_ID	10
#define USB_IN_PD_ID	11

#define CONTROL_ABSENT	0
#define CONTROL_RDONLY	1
#define CONTROL_RDWR	3

#define CLK_FREQ_CTRL	0
#define CLK_VLD_CTRL	2

#define INSRT_CTRL	0
#define OVRLD_CTRL	2
#define UNFLW_CTRL	4
#define OVFLW_CTRL	6

struct uac3_hc_desc {
	struct uac3_hc_descriptor_header	*hc_header;
	struct list_head			list;
};

struct f_uac3 {
	struct g_audio g_audio;

	/* High Capacity descriptors */
	struct list_head hc_desc_list;

	u8 ac_intf, as_in_intf, as_out_intf;
	u8 ac_alt, as_in_alt, as_out_alt;	/* needed for get_alt() */
};

static inline struct f_uac3 *func_to_uac3(struct usb_function *f)
{
	return container_of(f, struct f_uac3, g_audio.func);
}

static inline struct f_uac3_opts *g_audio_to_uac3_opts(struct g_audio *audio)
{
	return container_of(audio->func.fi, struct f_uac3_opts, func_inst);
}

/* --------- USB Function Interface ------------- */
enum {
	STR_ASSOC,
	STR_IF_CTRL,
	STR_AS_OUT_ALT0,
	STR_AS_OUT_ALT1,
	STR_AS_IN_ALT0,
	STR_AS_IN_ALT1,
};

static struct usb_string strings_fn[] = {
	[STR_ASSOC].s = "Source/Sink",
	[STR_IF_CTRL].s = "Topology Control",
	[STR_AS_OUT_ALT0].s = "Playback Inactive",
	[STR_AS_OUT_ALT1].s = "Playback Active",
	[STR_AS_IN_ALT0].s = "Capture Inactive",
	[STR_AS_IN_ALT1].s = "Capture Active",
	{ },
};

static struct usb_gadget_strings str_fn = {
	.language = 0x0409,	/* en-us */
	.strings = strings_fn,
};

static struct usb_gadget_strings *fn_strings[] = {
	&str_fn,
	NULL,
};

static struct usb_interface_assoc_descriptor iad_desc = {
	.bLength = sizeof iad_desc,
	.bDescriptorType = USB_DT_INTERFACE_ASSOCIATION,

	.bFirstInterface = 0,
	.bInterfaceCount = 3,
	.bFunctionClass = USB_CLASS_AUDIO,
	.bFunctionSubClass = UAC3_FUNCTION_SUBCLASS_GENERIC_IO,
	.bFunctionProtocol = UAC_VERSION_3,
};

/* Audio Control Interface */
static struct usb_interface_descriptor std_ac_if_desc = {
	.bLength = sizeof std_ac_if_desc,
	.bDescriptorType = USB_DT_INTERFACE,

	.bAlternateSetting = 0,
	.bNumEndpoints = 0,
	.bInterfaceClass = USB_CLASS_AUDIO,
	.bInterfaceSubClass = USB_SUBCLASS_AUDIOCONTROL,
	.bInterfaceProtocol = UAC_VERSION_3,
};

/* Clock source for IN traffic */
static struct uac3_clock_source_descriptor in_clk_src_desc = {
	.bLength = sizeof in_clk_src_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC3_CLOCK_SOURCE,
	.bClockID = USB_IN_CLK_ID,
	.bmAttributes = UAC3_CLOCK_SOURCE_TYPE_INT,
	.bmControls = cpu_to_le32(CONTROL_RDONLY << CLK_FREQ_CTRL),
	.bReferenceTerminal = 0,
	.wClockSourceStr = 0, /* Not used */
};

/* Clock source for OUT traffic */
static struct uac3_clock_source_descriptor out_clk_src_desc = {
	.bLength = sizeof out_clk_src_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC3_CLOCK_SOURCE,
	.bClockID = USB_OUT_CLK_ID,
	.bmAttributes = UAC3_CLOCK_SOURCE_TYPE_INT,
	.bmControls = cpu_to_le32(CONTROL_RDONLY << CLK_FREQ_CTRL),
	.bReferenceTerminal = 0,
	.wClockSourceStr = 0, /* Not used */
};

/* Input Terminal for USB_OUT */
static struct uac3_input_terminal_descriptor usb_out_it_desc = {
	.bLength = sizeof usb_out_it_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC_INPUT_TERMINAL,
	.bTerminalID = USB_OUT_IT_ID,
	.wTerminalType = cpu_to_le16(UAC_TERMINAL_STREAMING),
	.bAssocTerminal = 0,
	.bCSourceID = USB_OUT_CLK_ID,
	.bmControls = 0,
	.wClusterDescrID = 0, /* := dynamic */
	.wExTerminalDescrID = 0,
	.wConnectorsDescrID = 0,
	.wTerminalDescrStr = 0, /* Not used */
};

/* Output Terminal for I/O-Out */
static struct uac3_output_terminal_descriptor io_out_ot_desc = {
	.bLength = sizeof io_out_ot_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC_OUTPUT_TERMINAL,
	.bTerminalID = IO_OUT_OT_ID,
	.wTerminalType = cpu_to_le16(UAC_OUTPUT_TERMINAL_UNDEFINED),
	.bAssocTerminal = 0,
	.bSourceID = USB_OUT_FU_ID,
	.bCSourceID = USB_OUT_CLK_ID,
	.bmControls = 0,
	.wExTerminalDescrID = 0,
	.wConnectorsDescrID = 0,
	.wTerminalDescrStr = 0, /* Not used */
};

/* Input Terminal for I/O-In */
static struct uac3_input_terminal_descriptor io_in_it_desc = {
	.bLength = sizeof io_in_it_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC_INPUT_TERMINAL,
	.bTerminalID = IO_IN_IT_ID,
	.wTerminalType = cpu_to_le16(UAC_INPUT_TERMINAL_UNDEFINED),
	.bAssocTerminal = 0,
	.bCSourceID = USB_IN_CLK_ID,
	.bmControls = 0,
	.wClusterDescrID = 0, /* := dynamic */
	.wExTerminalDescrID = 0,
	.wConnectorsDescrID = 0,
	.wTerminalDescrStr = 0, /* Not used */
};

/* Output Terminal for USB_IN */
static struct uac3_output_terminal_descriptor usb_in_ot_desc = {
	.bLength = sizeof usb_in_ot_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC_OUTPUT_TERMINAL,
	.bTerminalID = USB_IN_OT_ID,
	.wTerminalType = cpu_to_le16(UAC_TERMINAL_STREAMING),
	.bAssocTerminal = 0,
	.bSourceID = USB_IN_FU_ID,
	.bCSourceID = USB_IN_CLK_ID,
	.bmControls = 0,
	.wExTerminalDescrID = 0,
	.wConnectorsDescrID = 0,
	.wTerminalDescrStr = 0, /* Not used */
};

/* Feature Units - dynamically allocated */
static struct uac3_feature_unit_descriptor *usb_out_fu_desc;
static struct uac3_feature_unit_descriptor *usb_in_fu_desc;

DECLARE_UAC3_POWER_DOMAIN_DESCRIPTOR(2);

/* Time to recover from D1 to D0. 30 us, expressed in 50 us increments */
#define BADP_RECOVERY_TIME_D1D0		0x0258
/* Time to recover from D2 to D0. 300 ms, expressed in 50 us increments. */
#define BADP_RECOVERY_TIME_D2D0		0x1770

static struct uac3_power_domain_descriptor_2 usb_out_pd_desc = {
	.bLength = sizeof usb_out_pd_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC3_POWER_DOMAIN,
	.bPowerDomainID = USB_OUT_PD_ID,
	.waRecoveryTime1 = cpu_to_le16(BADP_RECOVERY_TIME_D1D0),
	.waRecoveryTime2 = cpu_to_le16(BADP_RECOVERY_TIME_D2D0),
	.bNrEntities = 2,
	.baEntityID[0] = USB_OUT_IT_ID,
	.baEntityID[1] = IO_OUT_OT_ID,
	.wPDomainDescrStr = 0,	/* Not used */
};

static struct uac3_power_domain_descriptor_2 usb_in_pd_desc = {
	.bLength = sizeof usb_out_pd_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC3_POWER_DOMAIN,
	.bPowerDomainID = USB_IN_PD_ID,
	.waRecoveryTime1 = cpu_to_le16(BADP_RECOVERY_TIME_D1D0),
	.waRecoveryTime2 = cpu_to_le16(BADP_RECOVERY_TIME_D2D0),
	.bNrEntities = 2,
	.baEntityID[0] = IO_IN_IT_ID,
	.baEntityID[1] = USB_IN_OT_ID,
	.wPDomainDescrStr = 0,	/* Not used */
};

static struct uac3_ac_header_descriptor ac_hdr_desc = {
	.bLength = sizeof ac_hdr_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC_MS_HEADER,
	.bCategory = UAC3_FUNCTION_IO_BOX,
	/* .wTotalLength := DYNAMIC */
	.bmControls = 0,
};

/* Audio Streaming OUT Interface - Alt0 */
static struct usb_interface_descriptor std_as_out_if0_desc = {
	.bLength = sizeof std_as_out_if0_desc,
	.bDescriptorType = USB_DT_INTERFACE,

	.bAlternateSetting = 0,
	.bNumEndpoints = 0,
	.bInterfaceClass = USB_CLASS_AUDIO,
	.bInterfaceSubClass = USB_SUBCLASS_AUDIOSTREAMING,
	.bInterfaceProtocol = UAC_VERSION_3,
};

/* Audio Streaming OUT Interface - Alt1 */
static struct usb_interface_descriptor std_as_out_if1_desc = {
	.bLength = sizeof std_as_out_if1_desc,
	.bDescriptorType = USB_DT_INTERFACE,

	.bAlternateSetting = 1,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_AUDIO,
	.bInterfaceSubClass = USB_SUBCLASS_AUDIOSTREAMING,
	.bInterfaceProtocol = UAC_VERSION_3,
};

/* Audio Stream OUT Intface Desc */
static struct uac3_as_header_descriptor as_out_hdr_desc = {
	.bLength = sizeof as_out_hdr_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC_AS_GENERAL,
	.bTerminalLink = USB_OUT_IT_ID,
	.bmControls = 0,
	.wClusterDescrID = 0,
	.bmFormats = cpu_to_le64(UAC_FORMAT_TYPE_I_PCM),
	/* .bSubslotSize = DYNAMIC */
	/* .bBitResolution = DYNAMIC */
	.bmAuxProtocols = 0,
	.bControlSize = 0,
};

/* STD AS ISO OUT Endpoint */
static struct usb_endpoint_descriptor fs_epout_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_OUT,
	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC,
	.wMaxPacketSize = cpu_to_le16(1023),
	.bInterval = 1,
};

static struct usb_endpoint_descriptor hs_epout_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC,
	.wMaxPacketSize = cpu_to_le16(1024),
	.bInterval = 4,
};

/* CS AS ISO OUT Endpoint */
static struct uac3_iso_endpoint_descriptor as_iso_out_desc = {
	.bLength = sizeof as_iso_out_desc,
	.bDescriptorType = USB_DT_CS_ENDPOINT,

	.bDescriptorSubtype = UAC_EP_GENERAL,
	.bmControls = 0,
	.bLockDelayUnits = 0,
	.wLockDelay = 0,
};

/* Audio Streaming IN Interface - Alt0 */
static struct usb_interface_descriptor std_as_in_if0_desc = {
	.bLength = sizeof std_as_in_if0_desc,
	.bDescriptorType = USB_DT_INTERFACE,

	.bAlternateSetting = 0,
	.bNumEndpoints = 0,
	.bInterfaceClass = USB_CLASS_AUDIO,
	.bInterfaceSubClass = USB_SUBCLASS_AUDIOSTREAMING,
	.bInterfaceProtocol = UAC_VERSION_3,
};

/* Audio Streaming IN Interface - Alt1 */
static struct usb_interface_descriptor std_as_in_if1_desc = {
	.bLength = sizeof std_as_in_if1_desc,
	.bDescriptorType = USB_DT_INTERFACE,

	.bAlternateSetting = 1,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_AUDIO,
	.bInterfaceSubClass = USB_SUBCLASS_AUDIOSTREAMING,
	.bInterfaceProtocol = UAC_VERSION_3,
};

/* Audio Stream IN Intface Desc */
static struct uac3_as_header_descriptor as_in_hdr_desc = {
	.bLength = sizeof as_in_hdr_desc,
	.bDescriptorType = USB_DT_CS_INTERFACE,

	.bDescriptorSubtype = UAC_AS_GENERAL,
	.bTerminalLink = USB_IN_OT_ID,
	.bmControls = 0,
	.wClusterDescrID = 0,
	.bmFormats = cpu_to_le64(UAC_FORMAT_TYPE_I_PCM),
	/* .bSubslotSize = DYNAMIC */
	/* .bBitResolution = DYNAMIC */
	.bmAuxProtocols = 0,
	.bControlSize = 0,
};

/* STD AS ISO IN Endpoint */
static struct usb_endpoint_descriptor fs_epin_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC,
	.wMaxPacketSize = cpu_to_le16(1023),
	.bInterval = 1,
};

static struct usb_endpoint_descriptor hs_epin_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,

	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC,
	.wMaxPacketSize = cpu_to_le16(1024),
	.bInterval = 4,
};

/* CS AS ISO IN Endpoint */
static struct uac3_iso_endpoint_descriptor as_iso_in_desc = {
	.bLength = sizeof as_iso_in_desc,
	.bDescriptorType = USB_DT_CS_ENDPOINT,

	.bDescriptorSubtype = UAC_EP_GENERAL,
	.bmControls = 0,
	.bLockDelayUnits = 0,
	.wLockDelay = 0,
};

static struct usb_descriptor_header *fs_ac_audio_desc[] = {
	(struct usb_descriptor_header *)&iad_desc,
	(struct usb_descriptor_header *)&std_ac_if_desc,

	(struct usb_descriptor_header *)&ac_hdr_desc,
	(struct usb_descriptor_header *)&in_clk_src_desc,
	(struct usb_descriptor_header *)&out_clk_src_desc,
	(struct usb_descriptor_header *)&usb_out_it_desc,
	(struct usb_descriptor_header *)&io_in_it_desc,
	(struct usb_descriptor_header *)&usb_in_ot_desc,
	(struct usb_descriptor_header *)&io_out_ot_desc,
	(struct usb_descriptor_header *)&usb_in_pd_desc,
	(struct usb_descriptor_header *)&usb_out_pd_desc,
	NULL,
};

static struct usb_descriptor_header *fs_as_audio_desc[] = {
	(struct usb_descriptor_header *)&std_as_out_if0_desc,
	(struct usb_descriptor_header *)&std_as_out_if1_desc,

	(struct usb_descriptor_header *)&as_out_hdr_desc,
	(struct usb_descriptor_header *)&fs_epout_desc,
	(struct usb_descriptor_header *)&as_iso_out_desc,

	(struct usb_descriptor_header *)&std_as_in_if0_desc,
	(struct usb_descriptor_header *)&std_as_in_if1_desc,

	(struct usb_descriptor_header *)&as_in_hdr_desc,
	(struct usb_descriptor_header *)&fs_epin_desc,
	(struct usb_descriptor_header *)&as_iso_in_desc,
	NULL,
};

static struct usb_descriptor_header *hs_as_audio_desc[] = {
	(struct usb_descriptor_header *)&std_as_out_if0_desc,
	(struct usb_descriptor_header *)&std_as_out_if1_desc,

	(struct usb_descriptor_header *)&as_out_hdr_desc,
	(struct usb_descriptor_header *)&hs_epout_desc,
	(struct usb_descriptor_header *)&as_iso_out_desc,

	(struct usb_descriptor_header *)&std_as_in_if0_desc,
	(struct usb_descriptor_header *)&std_as_in_if1_desc,

	(struct usb_descriptor_header *)&as_in_hdr_desc,
	(struct usb_descriptor_header *)&hs_epin_desc,
	(struct usb_descriptor_header *)&as_iso_in_desc,
	NULL,
};

struct cntrl_cur_lay2 {
	__le16	wCUR;
};

struct cntrl_range_lay2 {
	__le16	wNumSubRanges;
	__le16	wMIN;
	__le16	wMAX;
	__le16	wRES;
} __packed;

struct cntrl_cur_lay3 {
	__le32	dCUR;
};

struct cntrl_range_lay3 {
	__le16	wNumSubRanges;
	__le32	dMIN;
	__le32	dMAX;
	__le32	dRES;
} __packed;

/*
 * Cluster descriptor we build here:
 *  +---------------------------------------+
 *  | Header                                |
 *  +---------------------------------------+
 *  |                 | Information segment |
 *  | Channel 1 block +---------------------+
 *  |                 | End segment         |
 *  +---------------------------------------+
 *  |                 ...                   |
 *  +---------------------------------------+
 *  |                 | Information segment |
 *  | Channel n block +---------------------+
 *  |                 | End segment         |
 *  +---------------------------------------+
 *
 *  FIXME: only mono and stereo channels supported at this time
 */
static void *build_cluster_descriptor(const struct f_uac3_opts *uac3_opts,
		bool is_playback)
{
	struct uac3_cluster_header_descriptor *cluster_desc;
	int i, chmask, nr_channels;
	u16 desc_size;
	void *p;

	if (is_playback)
		chmask = uac3_opts->p_chmask;
	else
		chmask = uac3_opts->c_chmask;

	nr_channels = num_channels(chmask);

	if (!nr_channels) {
		pr_err("f_uac3: no channels\n");
		return NULL;
	}

	if (chmask & ~(0x3)) {
		pr_err("f_uac3: only mono/stereo channels supported\n");
		return NULL;
	}

	desc_size = sizeof(struct uac3_cluster_header_descriptor) +
		nr_channels *
		(sizeof(struct uac3_cluster_information_segment_descriptor) +
			sizeof(struct uac3_cluster_end_segment_descriptor));

	cluster_desc = kzalloc(desc_size, GFP_KERNEL);
	if (!cluster_desc)
		return NULL;

	cluster_desc->wLength = cpu_to_le16(desc_size);
	cluster_desc->bDescriptorType = UAC3_CS_CLUSTER;
	cluster_desc->bDescriptorSubtype = UAC3_SEGMENT_UNDEFINED;
	cluster_desc->bNrChannels = nr_channels;

	p = cluster_desc;
	p += sizeof(struct uac3_cluster_header_descriptor);
	for (i = 0; i < nr_channels; i++) {
		struct uac3_cluster_information_segment_descriptor *is_desc;
		struct uac3_cluster_end_segment_descriptor *es_desc;
		u8 ch_relationship;

		is_desc = p;
		is_desc->wLength = cpu_to_le16(sizeof(*is_desc));
		is_desc->bSegmentType = UAC3_CHANNEL_INFORMATION;
		is_desc->bChPurpose = UAC3_PURPOSE_GENERIC_AUDIO;

		switch (nr_channels) {
		case 1:
		default:
			ch_relationship = UAC3_CH_MONO;
			break;
		case 2:
			if (chmask & 1)
				ch_relationship = UAC3_CH_LEFT;
			else
				ch_relationship = UAC3_CH_RIGHT;
			break;
		}

		is_desc->bChRelationship = ch_relationship;
		is_desc->bChGroupID = 0;

		p += sizeof(struct uac3_cluster_information_segment_descriptor);
		es_desc = p;
		es_desc->wLength = cpu_to_le16(sizeof(*es_desc));
		es_desc->bSegmentType = UAC3_END_SEGMENT;
	}
	return cluster_desc;
}

static struct uac3_feature_unit_descriptor *alloc_fu_desc(unsigned int ch,
							  u8 unit_id,
							  u8 source_id)
{
	struct uac3_feature_unit_descriptor *fu_desc;
	__le32 *bma_control;

	fu_desc = kzalloc(UAC3_DT_FEATURE_UNIT_SIZE(ch), GFP_KERNEL);
	if (!fu_desc)
		return NULL;

	fu_desc->bLength = UAC3_DT_FEATURE_UNIT_SIZE(ch);
	fu_desc->bDescriptorType = USB_DT_CS_INTERFACE;
	fu_desc->bDescriptorSubtype = UAC3_FEATURE_UNIT;
	fu_desc->bUnitID = unit_id;
	fu_desc->bSourceID = source_id;

	bma_control = (__le32 *)&fu_desc->bmaControls;

	/* REVISIT: currently hardcoded as described in BADP spec */
	/* Master Channel: Only a Mute Control shall be present */
	*bma_control++ = cpu_to_le32(CONTROL_RDWR << ((UAC_FU_MUTE - 1) * 2));
	/* Channel 1+ : Only a Volume Control shall be present  */
	while (ch--)
		*bma_control++ = cpu_to_le32(CONTROL_RDWR << ((UAC_FU_VOLUME - 1) * 2));

	/* fu_desc->wFeatureDescrStr := Not used */

	return fu_desc;
}

static void set_ep_max_packet_size(const struct f_uac3_opts *uac3_opts,
				   struct usb_endpoint_descriptor *ep_desc,
				   unsigned int factor, bool is_playback)
{
	int chmask, srate, ssize;
	u16 max_packet_size;

	if (is_playback) {
		chmask = uac3_opts->p_chmask;
		srate = uac3_opts->p_srate;
		ssize = uac3_opts->p_ssize;
	} else {
		chmask = uac3_opts->c_chmask;
		srate = uac3_opts->c_srate;
		ssize = uac3_opts->c_ssize;
	}

	max_packet_size = num_channels(chmask) * ssize *
		DIV_ROUND_UP(srate, factor / (1 << (ep_desc->bInterval - 1)));
	ep_desc->wMaxPacketSize = cpu_to_le16(min_t(u16, max_packet_size,
				le16_to_cpu(ep_desc->wMaxPacketSize)));
}

#define UAC3_COPY_DESCRIPTOR(mem, dst, desc) \
	do { \
		memcpy(mem, desc, (desc)->bLength); \
		*(dst)++ = mem; \
		mem += (desc)->bLength; \
	} while (0)

static struct usb_descriptor_header **
uac3_copy_descriptors(enum usb_device_speed speed)
{
	struct usb_descriptor_header **uac3_control_desc = fs_ac_audio_desc;
	struct usb_descriptor_header **uac3_streaming_desc;

	struct usb_descriptor_header **tmp;
	unsigned bytes;
	unsigned n_desc;
	void *mem;
	struct usb_descriptor_header **ret;

	switch (speed) {
	case USB_SPEED_HIGH:
		uac3_streaming_desc = hs_as_audio_desc;
		break;

	case USB_SPEED_FULL:
	default:
		uac3_streaming_desc = fs_as_audio_desc;
		break;
	}

	/* Count descriptors and their sizes */
	for (bytes = 0, n_desc = 0, tmp = uac3_control_desc; *tmp; tmp++, n_desc++)
		bytes += (*tmp)->bLength;
	if (usb_out_fu_desc) {
		bytes += usb_out_fu_desc->bLength;
		n_desc++;
	}
	if (usb_in_fu_desc) {
		bytes += usb_in_fu_desc->bLength;
		n_desc++;
	}
	for (tmp = uac3_streaming_desc; *tmp; tmp++, n_desc++)
		bytes += (*tmp)->bLength;
	bytes += (n_desc + 1) * sizeof(*tmp);

	mem = kmalloc(bytes, GFP_KERNEL);
	if (!mem)
		return NULL;

	/* fill in pointers starting at "tmp",
	 * to descriptors copied starting at "mem";
	 * and return "ret"
	 */
	tmp = mem;
	ret = mem;
	mem += (n_desc + 1) * sizeof(*tmp);
	while (*uac3_control_desc) {
		memcpy(mem, *uac3_control_desc, (*uac3_control_desc)->bLength);
		*tmp = mem;
		tmp++;
		mem += (*uac3_control_desc)->bLength;
		uac3_control_desc++;
	}

	if (usb_out_fu_desc)
		UAC3_COPY_DESCRIPTOR(mem, tmp, usb_out_fu_desc);
	if (usb_in_fu_desc)
		UAC3_COPY_DESCRIPTOR(mem, tmp, usb_in_fu_desc);

	while (*uac3_streaming_desc) {
		memcpy(mem, *uac3_streaming_desc, (*uac3_streaming_desc)->bLength);
		*tmp = mem;
		tmp++;
		mem += (*uac3_streaming_desc)->bLength;
		uac3_streaming_desc++;
	}
	*tmp = NULL;

	return ret;
}

static int f_audio_bind(struct usb_configuration *cfg, struct usb_function *fn)
{
	struct f_uac3 *uac3 = func_to_uac3(fn);
	struct g_audio *audio = func_to_g_audio(fn);
	struct usb_composite_dev *cdev = cfg->cdev;
	struct usb_gadget *gadget = cdev->gadget;
	struct device *dev = &gadget->dev;
	struct f_uac3_opts *uac3_opts;
	struct uac3_hc_descriptor_header *cluster_desc;
	struct uac3_hc_desc *hc_desc;
	struct usb_string *us;
	u16 hc_desc_id = 1; /* HC id always starts from 1 */
	int ret;

	uac3_opts = container_of(fn->fi, struct f_uac3_opts, func_inst);

	us = usb_gstrings_attach(cdev, fn_strings, ARRAY_SIZE(strings_fn));
	if (IS_ERR(us))
		return PTR_ERR(us);

	iad_desc.iFunction = us[STR_ASSOC].id;
	std_ac_if_desc.iInterface = us[STR_IF_CTRL].id;
	std_as_out_if0_desc.iInterface = us[STR_AS_OUT_ALT0].id;
	std_as_out_if1_desc.iInterface = us[STR_AS_OUT_ALT1].id;
	std_as_in_if0_desc.iInterface = us[STR_AS_IN_ALT0].id;
	std_as_in_if1_desc.iInterface = us[STR_AS_IN_ALT1].id;

	INIT_LIST_HEAD(&uac3->hc_desc_list);

	/* Initialize the configurable parameters */
	cluster_desc = build_cluster_descriptor(uac3_opts, 0); /* capture */
	if (cluster_desc) {
		hc_desc = kzalloc(sizeof *hc_desc, GFP_KERNEL);
		hc_desc->hc_header = cluster_desc;
		list_add(&hc_desc->list, &uac3->hc_desc_list);
		cluster_desc->wDescriptorID = cpu_to_le16(hc_desc_id);
		usb_out_it_desc.wClusterDescrID = cluster_desc->wDescriptorID;
		as_out_hdr_desc.wClusterDescrID = cluster_desc->wDescriptorID;
		hc_desc_id++;
	}

	cluster_desc = build_cluster_descriptor(uac3_opts, 1); /* playback */
	if (cluster_desc) {
		hc_desc = kzalloc(sizeof *hc_desc, GFP_KERNEL);
		hc_desc->hc_header = cluster_desc;
		list_add(&hc_desc->list, &uac3->hc_desc_list);
		cluster_desc->wDescriptorID = cpu_to_le16(hc_desc_id);
		io_in_it_desc.wClusterDescrID = cluster_desc->wDescriptorID;
		as_in_hdr_desc.wClusterDescrID = cluster_desc->wDescriptorID;
	}

	as_out_hdr_desc.bSubslotSize = uac3_opts->c_ssize;
	as_out_hdr_desc.bBitResolution = uac3_opts->c_ssize * 8;
	as_in_hdr_desc.bSubslotSize = uac3_opts->p_ssize;
	as_in_hdr_desc.bBitResolution = uac3_opts->p_ssize * 8;

	/* alloc and configure Feature Unit descriptors */
	usb_out_fu_desc = alloc_fu_desc(num_channels(uac3_opts->c_chmask),
					USB_OUT_FU_ID,
					USB_OUT_IT_ID);
	if (!usb_out_fu_desc) {
		dev_err(dev, "%s: can't allocate OUT FU descriptor on %s\n",
				 fn->name, gadget->name);
		ret = -ENOMEM;
		goto err_free_hc_desc;
	}

	usb_in_fu_desc = alloc_fu_desc(num_channels(uac3_opts->p_chmask),
					USB_IN_FU_ID,
					IO_IN_IT_ID);
	if (!usb_in_fu_desc) {
		dev_err(dev, "%s: can't allocate IN FU descriptor on %s\n",
				 fn->name, gadget->name);
		ret = -ENOMEM;
		goto err_free_out_fu_desc;
	}

	/* update AC desc size with allocated FUs */
	ac_hdr_desc.wTotalLength = cpu_to_le16(
			  sizeof in_clk_src_desc + sizeof out_clk_src_desc
			+ sizeof usb_out_it_desc + sizeof io_in_it_desc
			+ sizeof usb_in_ot_desc + sizeof io_out_ot_desc
			+ sizeof usb_in_pd_desc + sizeof usb_out_pd_desc
			+ usb_out_fu_desc->bLength + usb_in_fu_desc->bLength);

	ret = usb_interface_id(cfg, fn);
	if (ret < 0) {
		dev_err(dev, "%s: can't allocate AC interface id on %s\n",
				 fn->name, gadget->name);
		goto err_free_in_fu_desc;
	}
	std_ac_if_desc.bInterfaceNumber = ret;
	uac3->ac_intf = ret;
	uac3->ac_alt = 0;

	ret = usb_interface_id(cfg, fn);
	if (ret < 0) {
		dev_err(dev, "%s: can't allocate AS OUT interface id on %s\n",
				 fn->name, gadget->name);
		goto err_free_in_fu_desc;
	}
	std_as_out_if0_desc.bInterfaceNumber = ret;
	std_as_out_if1_desc.bInterfaceNumber = ret;
	uac3->as_out_intf = ret;
	uac3->as_out_alt = 0;

	ret = usb_interface_id(cfg, fn);
	if (ret < 0) {
		dev_err(dev, "%s: can't allocate AS IN interface id on %s\n",
				 fn->name, gadget->name);
		goto err_free_in_fu_desc;
	}
	std_as_in_if0_desc.bInterfaceNumber = ret;
	std_as_in_if1_desc.bInterfaceNumber = ret;
	uac3->as_in_intf = ret;
	uac3->as_in_alt = 0;

	/* Calculate wMaxPacketSize according to audio bandwidth */
	set_ep_max_packet_size(uac3_opts, &fs_epin_desc, 1000, true);
	set_ep_max_packet_size(uac3_opts, &fs_epout_desc, 1000, false);
	set_ep_max_packet_size(uac3_opts, &hs_epin_desc, 8000, true);
	set_ep_max_packet_size(uac3_opts, &hs_epout_desc, 8000, false);

	audio->out_ep = usb_ep_autoconfig(gadget, &fs_epout_desc);
	if (!audio->out_ep) {
		dev_err(dev, "%s: can't autoconfigure on %s\n",
				 fn->name, gadget->name);
		ret = -ENODEV;
		goto err_free_in_fu_desc;
	}

	audio->in_ep = usb_ep_autoconfig(gadget, &fs_epin_desc);
	if (!audio->in_ep) {
		dev_err(dev, "%s: can't autoconfigure on %s\n",
				 fn->name, gadget->name);
		ret = -ENODEV;
		goto err_free_in_fu_desc;
	}

	audio->in_ep_maxpsize = max_t(u16,
				le16_to_cpu(fs_epin_desc.wMaxPacketSize),
				le16_to_cpu(hs_epin_desc.wMaxPacketSize));
	audio->out_ep_maxpsize = max_t(u16,
				le16_to_cpu(fs_epout_desc.wMaxPacketSize),
				le16_to_cpu(hs_epout_desc.wMaxPacketSize));

	hs_epout_desc.bEndpointAddress = fs_epout_desc.bEndpointAddress;
	hs_epin_desc.bEndpointAddress = fs_epin_desc.bEndpointAddress;

	/* Copy descriptors */
	fn->fs_descriptors = uac3_copy_descriptors(USB_SPEED_FULL);
	if (!fn->fs_descriptors)
		goto err_free_in_fu_desc;
	if (gadget_is_dualspeed(gadget)) {
		fn->hs_descriptors = uac3_copy_descriptors(USB_SPEED_HIGH);
		if (!fn->hs_descriptors)
			goto err_free_in_fu_desc;
	}

	audio->gadget = gadget;

	audio->params.p_chmask = uac3_opts->p_chmask;
	audio->params.p_srate = uac3_opts->p_srate;
	audio->params.p_ssize = uac3_opts->p_ssize;
	audio->params.c_chmask = uac3_opts->c_chmask;
	audio->params.c_srate = uac3_opts->c_srate;
	audio->params.c_ssize = uac3_opts->c_ssize;
	audio->params.req_number = uac3_opts->req_number;
	ret = g_audio_setup(audio, "UAC3 PCM", "UAC3_Gadget");
	if (ret)
		goto err_free_descs;
	return 0;

err_free_descs:
	usb_free_all_descriptors(fn);
	audio->gadget = NULL;
err_free_in_fu_desc:
	kfree(usb_in_fu_desc);
	usb_in_fu_desc = NULL;
err_free_out_fu_desc:
	kfree(usb_out_fu_desc);
	usb_out_fu_desc = NULL;
err_free_hc_desc:
	list_for_each_entry(hc_desc, &uac3->hc_desc_list, list)
		kfree(hc_desc);

	return ret;
}

static void f_audio_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct g_audio *audio = func_to_g_audio(f);
	struct f_uac3 *uac3 = func_to_uac3(f);
	struct uac3_hc_desc *hc_desc;

	g_audio_cleanup(audio);
	usb_free_all_descriptors(f);
	audio->gadget = NULL;
	kfree(usb_in_fu_desc);
	usb_in_fu_desc = NULL;
	kfree(usb_out_fu_desc);
	usb_out_fu_desc = NULL;

	list_for_each_entry(hc_desc, &uac3->hc_desc_list, list)
		kfree(hc_desc);
}

static int f_audio_set_alt(struct usb_function *fn, unsigned intf, unsigned alt)
{
	struct usb_composite_dev *cdev = fn->config->cdev;
	struct f_uac3 *uac3 = func_to_uac3(fn);
	struct usb_gadget *gadget = cdev->gadget;
	struct device *dev = &gadget->dev;
	int ret = 0;

	/* No i/f has more than 2 alt settings */
	if (alt > 1) {
		dev_err(dev, "%s: Invalid altsetting %d\n", fn->name, alt);
		return -EINVAL;
	}

	if (intf == uac3->ac_intf) {
		/* Control I/f has only 1 AltSetting - 0 */
		if (alt) {
			dev_err(dev,
				"%s: Invalid Control I/f altsetting %d\n",
				fn->name, alt);
			return -EINVAL;
		}
		return 0;
	}

	if (intf == uac3->as_out_intf) {
		uac3->as_out_alt = alt;

		if (alt)
			ret = u_audio_start_capture(&uac3->g_audio);
		else
			u_audio_stop_capture(&uac3->g_audio);
	} else if (intf == uac3->as_in_intf) {
		uac3->as_in_alt = alt;

		if (alt)
			ret = u_audio_start_playback(&uac3->g_audio);
		else
			u_audio_stop_playback(&uac3->g_audio);
	} else {
		dev_err(dev, "%s: Invalid interface %d\n", fn->name, intf);
		return -EINVAL;
	}

	return ret;
}

static int f_audio_get_alt(struct usb_function *fn, unsigned intf)
{
	struct f_uac3 *uac3 = func_to_uac3(fn);
	struct g_audio *audio = func_to_g_audio(fn);

	if (intf == uac3->ac_intf)
		return uac3->ac_alt;
	else if (intf == uac3->as_out_intf)
		return uac3->as_out_alt;
	else if (intf == uac3->as_in_intf)
		return uac3->as_in_alt;
	else
		dev_err(&audio->gadget->dev, "%s: Invalid interface %d\n",
							fn->name, intf);

	return -EINVAL;
}

static void f_audio_disable(struct usb_function *fn)
{
	struct f_uac3 *uac3 = func_to_uac3(fn);

	uac3->as_in_alt = 0;
	uac3->as_out_alt = 0;
	u_audio_stop_capture(&uac3->g_audio);
	u_audio_stop_playback(&uac3->g_audio);
}

static int in_rq_cur(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct usb_request *req = fn->config->cdev->req;
	struct g_audio *audio = func_to_g_audio(fn);
	struct f_uac3_opts *opts;
	u16 w_length = le16_to_cpu(cr->wLength);
	u16 w_index = le16_to_cpu(cr->wIndex);
	u16 w_value = le16_to_cpu(cr->wValue);
	u8 entity_id = (w_index >> 8) & 0xff;
	u8 control_selector = w_value >> 8;
	int value = -EOPNOTSUPP;
	int p_srate, c_srate;

	opts = g_audio_to_uac3_opts(audio);
	p_srate = opts->p_srate;
	c_srate = opts->c_srate;

	switch (entity_id) {
	case USB_IN_CLK_ID:
	case USB_OUT_CLK_ID:
		if (control_selector == UAC2_CS_CONTROL_SAM_FREQ) {
			struct cntrl_cur_lay3 c;
			memset(&c, 0, sizeof(struct cntrl_cur_lay3));

			if (entity_id == USB_IN_CLK_ID)
				c.dCUR = cpu_to_le32(p_srate);
			else if (entity_id == USB_OUT_CLK_ID)
				c.dCUR = cpu_to_le32(c_srate);

			value = min_t(unsigned, w_length, sizeof c);
			memcpy(req->buf, &c, value);
		} else if (control_selector == UAC2_CS_CONTROL_CLOCK_VALID) {
			*(u8 *)req->buf = 1;
			value = min_t(unsigned, w_length, 1);
		} else {
			dev_err(&audio->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
		}
		break;

	case USB_OUT_PD_ID:
	case USB_IN_PD_ID:
		if (control_selector == UAC3_AC_POWER_DOMAIN_CONTROL) {
			/* FIXME: hardcoded to Power Domain State D0 */
			*(u8 *)req->buf = 0;
			value = min_t(unsigned, w_length, 1);
		} else {
			dev_err(&audio->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
		}
		break;

	case USB_IN_FU_ID:
	case USB_OUT_FU_ID:
		if (control_selector == UAC_FU_MUTE) {
			/* FIXME: hardcoded to false (not muted) */
			*(u8 *)req->buf = 0;
			value = min_t(unsigned, w_length, 1);
		} else if (control_selector == UAC_FU_VOLUME) {
			struct cntrl_cur_lay2 r;

			/* FIXME: hardcoded to 0dB */
			r.wCUR = 0;

			value = min_t(unsigned, w_length, sizeof r);
			memcpy(req->buf, &r, value);
		} else {
			dev_err(&audio->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
		}
		break;

	default:
		value = -EOPNOTSUPP;
		break;
	}

	return value;
}

static int
in_rq_range(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct usb_request *req = fn->config->cdev->req;
	struct g_audio *audio = func_to_g_audio(fn);
	struct f_uac3_opts *opts;
	u16 w_length = le16_to_cpu(cr->wLength);
	u16 w_index = le16_to_cpu(cr->wIndex);
	u16 w_value = le16_to_cpu(cr->wValue);
	u8 entity_id = (w_index >> 8) & 0xff;
	u8 control_selector = w_value >> 8;
	int value = -EOPNOTSUPP;
	int p_srate, c_srate;

	opts = g_audio_to_uac3_opts(audio);
	p_srate = opts->p_srate;
	c_srate = opts->c_srate;


	switch (entity_id) {
	case USB_IN_CLK_ID:
	case USB_OUT_CLK_ID: {
		if (control_selector == UAC2_CS_CONTROL_SAM_FREQ) {
			struct cntrl_range_lay3 r;

			if (entity_id == USB_IN_CLK_ID)
				r.dMIN = cpu_to_le32(p_srate);
			else if (entity_id == USB_OUT_CLK_ID)
				r.dMIN = cpu_to_le32(c_srate);
			else
				return -EOPNOTSUPP;

			r.dMAX = r.dMIN;
			r.dRES = 0;
			r.wNumSubRanges = cpu_to_le16(1);

			value = min_t(unsigned, w_length, sizeof r);
			memcpy(req->buf, &r, value);
		} else {
			dev_err(&audio->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
		}
		break;
	}
	case USB_IN_FU_ID:
	case USB_OUT_FU_ID: {
		if (control_selector == UAC_FU_VOLUME) {
			struct cntrl_range_lay2 r;

			r.wMIN = cpu_to_le16(0x8001); /* -127.9961 dB */
			r.wMAX = 0; /* 0 dB */
			r.wRES = cpu_to_le16(0x0001); /* in steps of 1/256 dB */
			r.wNumSubRanges = cpu_to_le16(1);

			value = min_t(unsigned, w_length, sizeof r);
			memcpy(req->buf, &r, value);
		} else {
			dev_err(&audio->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
		}
		break;
	}
	default:
		dev_err(&audio->gadget->dev,
			"%s:%d control_selector=%d TODO!\n",
			__func__, __LINE__, control_selector);

	}


	return value;
}

static int out_rq_cur(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct g_audio *audio = func_to_g_audio(fn);
	u16 w_length = le16_to_cpu(cr->wLength);
	u16 w_value = le16_to_cpu(cr->wValue);
	u16 w_index = le16_to_cpu(cr->wIndex);
	u8 entity_id = (w_index >> 8) & 0xff;
	u8 control_selector = w_value >> 8;
	int value = -EOPNOTSUPP;

	/* TODO: implement real clock/mute/volume handling */
	switch (entity_id) {
	case USB_IN_CLK_ID:
	case USB_OUT_CLK_ID:
		if (control_selector == UAC2_CS_CONTROL_SAM_FREQ)
			value = w_length;
		break;

	case USB_IN_FU_ID:
	case USB_OUT_FU_ID:
		if ((control_selector == UAC_FU_MUTE)
				|| (control_selector == UAC_FU_VOLUME))
			value = w_length;
		break;

	default:
		dev_err(&audio->gadget->dev,
			"%s:%d control_selector=%d TODO!\n",
			__func__, __LINE__, control_selector);
	}

	return value;
}

static int
in_rq_hc_desc(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct f_uac3 *uac3 = func_to_uac3(fn);
	struct g_audio *audio = func_to_g_audio(fn);
	struct usb_request *req = fn->config->cdev->req;
	u16 w_length = le16_to_cpu(cr->wLength);
	u16 w_value = le16_to_cpu(cr->wValue);
	struct uac3_hc_desc *hc_desc;
	u16 hc_desc_len;
	int value;

	list_for_each_entry(hc_desc, &uac3->hc_desc_list, list) {
		u16 w_desc_id;

		w_desc_id = le16_to_cpu(hc_desc->hc_header->wDescriptorID);
		if (w_desc_id == w_value)
			goto found;
	}
	dev_err(&audio->gadget->dev, "No High Capability descriptor %d\n",
				     w_value);
	return -EOPNOTSUPP;

found:
	hc_desc_len = le16_to_cpu(hc_desc->hc_header->wLength);
	value = min_t(unsigned, w_length, hc_desc_len);
	memcpy(req->buf, hc_desc->hc_header, value);

	return value;
}

static int ac_rq_in(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	if (cr->bRequest == UAC3_CS_REQ_CUR)
		return in_rq_cur(fn, cr);
	else if (cr->bRequest == UAC3_CS_REQ_RANGE)
		return in_rq_range(fn, cr);
	else if (cr->bRequest == UAC3_CS_REQ_HIGH_CAPABILITY_DESCRIPTOR)
		return in_rq_hc_desc(fn, cr);
	else
		return -EOPNOTSUPP;
}

static int
setup_rq_inf(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct f_uac3 *uac3 = func_to_uac3(fn);
	struct g_audio *audio = func_to_g_audio(fn);
	u16 w_index = le16_to_cpu(cr->wIndex);
	u8 intf = w_index & 0xff;

	if (intf != uac3->ac_intf) {
		dev_err(&audio->gadget->dev,
			"%s:%d Error!\n", __func__, __LINE__);
		return -EOPNOTSUPP;
	}

	if (cr->bRequestType & USB_DIR_IN)
		return ac_rq_in(fn, cr);
	else if (cr->bRequest == UAC3_CS_REQ_CUR)
		return out_rq_cur(fn, cr);

	return -EOPNOTSUPP;
}

static int
f_audio_setup(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct usb_composite_dev *cdev = fn->config->cdev;
	struct g_audio *audio = func_to_g_audio(fn);
	struct usb_request *req = cdev->req;
	u16 w_length = le16_to_cpu(cr->wLength);
	int value = -EOPNOTSUPP;

	/* Only Class specific requests are supposed to reach here */
	if ((cr->bRequestType & USB_TYPE_MASK) != USB_TYPE_CLASS)
		return -EOPNOTSUPP;

	if ((cr->bRequestType & USB_RECIP_MASK) == USB_RECIP_INTERFACE)
		value = setup_rq_inf(fn, cr);
	else
		dev_err(&audio->gadget->dev, "%s:%d Error!\n",
				__func__, __LINE__);

	if (value >= 0) {
		req->length = value;
		req->zero = value < w_length;
		value = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
		if (value < 0) {
			dev_err(&audio->gadget->dev,
				"%s:%d Error!\n", __func__, __LINE__);
			req->status = 0;
		}
	}

	return value;
}

static inline struct f_uac3_opts *to_f_uac3_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct f_uac3_opts,
			    func_inst.group);
}

static void f_uac3_attr_release(struct config_item *item)
{
	struct f_uac3_opts *opts = to_f_uac3_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations f_uac3_item_ops = {
	.release	= f_uac3_attr_release,
};

#define UAC3_ATTRIBUTE_CHMASK(name)					\
static ssize_t f_uac3_opts_##name##_show(struct config_item *item,	\
					 char *page)			\
{									\
	struct f_uac3_opts *opts = to_f_uac3_opts(item);		\
	int result;							\
									\
	mutex_lock(&opts->lock);					\
	result = sprintf(page, "%u\n", opts->name);			\
	mutex_unlock(&opts->lock);					\
									\
	return result;							\
}									\
									\
static ssize_t f_uac3_opts_##name##_store(struct config_item *item,	\
					  const char *page, size_t len)	\
{									\
	struct f_uac3_opts *opts = to_f_uac3_opts(item);		\
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
	/* FIXME: only mono/stereo supported at this time */		\
	if (num & (~0x3)) {						\
		ret = -EINVAL;						\
		goto end;						\
	}								\
									\
	opts->name = num;						\
	ret = len;							\
									\
end:									\
	mutex_unlock(&opts->lock);					\
	return ret;							\
}									\
									\
CONFIGFS_ATTR(f_uac3_opts_, name)

#define UAC3_ATTRIBUTE(name)						\
static ssize_t f_uac3_opts_##name##_show(struct config_item *item,	\
					 char *page)			\
{									\
	struct f_uac3_opts *opts = to_f_uac3_opts(item);		\
	int result;							\
									\
	mutex_lock(&opts->lock);					\
	result = sprintf(page, "%u\n", opts->name);			\
	mutex_unlock(&opts->lock);					\
									\
	return result;							\
}									\
									\
static ssize_t f_uac3_opts_##name##_store(struct config_item *item,	\
					  const char *page, size_t len)	\
{									\
	struct f_uac3_opts *opts = to_f_uac3_opts(item);		\
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
CONFIGFS_ATTR(f_uac3_opts_, name)

UAC3_ATTRIBUTE_CHMASK(p_chmask);
UAC3_ATTRIBUTE(p_srate);
UAC3_ATTRIBUTE(p_ssize);
UAC3_ATTRIBUTE_CHMASK(c_chmask);
UAC3_ATTRIBUTE(c_srate);
UAC3_ATTRIBUTE(c_ssize);
UAC3_ATTRIBUTE(req_number);

static struct configfs_attribute *f_uac3_attrs[] = {
	&f_uac3_opts_attr_p_chmask,
	&f_uac3_opts_attr_p_srate,
	&f_uac3_opts_attr_p_ssize,
	&f_uac3_opts_attr_c_chmask,
	&f_uac3_opts_attr_c_srate,
	&f_uac3_opts_attr_c_ssize,
	&f_uac3_opts_attr_req_number,
	NULL,
};

static struct config_item_type f_uac3_func_type = {
	.ct_item_ops	= &f_uac3_item_ops,
	.ct_attrs	= f_uac3_attrs,
	.ct_owner	= THIS_MODULE,
};

static void f_audio_free_inst(struct usb_function_instance *f)
{
	struct f_uac3_opts *opts;

	opts = container_of(f, struct f_uac3_opts, func_inst);
	kfree(opts);
}

static struct usb_function_instance *f_audio_alloc_inst(void)
{
	struct f_uac3_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	mutex_init(&opts->lock);
	opts->func_inst.free_func_inst = f_audio_free_inst;

	config_group_init_type_name(&opts->func_inst.group, "",
				    &f_uac3_func_type);

	opts->p_chmask = UAC3_DEF_PCHMASK;
	opts->p_srate = UAC3_DEF_PSRATE;
	opts->p_ssize = UAC3_DEF_PSSIZE;
	opts->c_chmask = UAC3_DEF_CCHMASK;
	opts->c_srate = UAC3_DEF_CSRATE;
	opts->c_ssize = UAC3_DEF_CSSIZE;
	opts->req_number = UAC3_DEF_REQ_NUM;
	return &opts->func_inst;
}

static void f_audio_free(struct usb_function *f)
{
	struct g_audio *audio;
	struct f_uac3_opts *opts;

	audio = func_to_g_audio(f);
	opts = container_of(f->fi, struct f_uac3_opts, func_inst);
	kfree(audio);
	mutex_lock(&opts->lock);
	--opts->refcnt;
	mutex_unlock(&opts->lock);
}

static struct usb_function *f_audio_alloc(struct usb_function_instance *fi)
{
	struct f_uac3	*uac3;
	struct f_uac3_opts *opts;

	uac3 = kzalloc(sizeof(*uac3), GFP_KERNEL);
	if (!uac3)
		return ERR_PTR(-ENOMEM);

	opts = container_of(fi, struct f_uac3_opts, func_inst);
	mutex_lock(&opts->lock);
	++opts->refcnt;
	mutex_unlock(&opts->lock);

	uac3->g_audio.func.name = "uac3_func";
	uac3->g_audio.func.bind = f_audio_bind;
	uac3->g_audio.func.unbind = f_audio_unbind;
	uac3->g_audio.func.set_alt = f_audio_set_alt;
	uac3->g_audio.func.get_alt = f_audio_get_alt;
	uac3->g_audio.func.disable = f_audio_disable;
	uac3->g_audio.func.setup = f_audio_setup;
	uac3->g_audio.func.free_func = f_audio_free;

	return &uac3->g_audio.func;
}

DECLARE_USB_FUNCTION_INIT(uac3, f_audio_alloc_inst, f_audio_alloc);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ruslan Bilovol");
