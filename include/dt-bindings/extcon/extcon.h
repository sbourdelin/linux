/*
 * This header provides constants for most extcon bindings.
 *
 * Most extcon bindings include the unique identification format.
 * In most cases, the unique id format uses the standard values/macro
 * defined in this header.
 */

#ifndef _DT_BINDINGS_EXTCON_EXTCON_H
#define _DT_BINDINGS_EXTCON_EXTCON_H

/* USB external connector */
#define EXTCON_USB		1
#define EXTCON_USB_HOST		2

/* Charging external connector */
#define EXTCON_CHG_USB_SDP	5	/* Standard Downstream Port */
#define EXTCON_CHG_USB_DCP	6	/* Dedicated Charging Port */
#define EXTCON_CHG_USB_CDP	7	/* Charging Downstream Port */
#define EXTCON_CHG_USB_ACA	8	/* Accessory Charger Adapter */
#define EXTCON_CHG_USB_FAST	9
#define EXTCON_CHG_USB_SLOW	10

/* Jack external connector */
#define EXTCON_JACK_MICROPHONE	20
#define EXTCON_JACK_HEADPHONE	21
#define EXTCON_JACK_LINE_IN	22
#define EXTCON_JACK_LINE_OUT	23
#define EXTCON_JACK_VIDEO_IN	24
#define EXTCON_JACK_VIDEO_OUT	25
#define EXTCON_JACK_SPDIF_IN	26	/* Sony Philips Digital InterFace */
#define EXTCON_JACK_SPDIF_OUT	27

/* Display external connector */
#define EXTCON_DISP_HDMI	40	/* High-Definition Multimedia Interface */
#define EXTCON_DISP_MHL		41	/* Mobile High-Definition Link */
#define EXTCON_DISP_DVI		42	/* Digital Visual Interface */
#define EXTCON_DISP_VGA		43	/* Video Graphics Array */

/* Miscellaneous external connector */
#define EXTCON_DOCK		60
#define EXTCON_JIG		61
#define EXTCON_MECHANICAL	62

#define EXTCON_NUM		63

#endif /* _DT_BINDINGS_EXTCON_EXTCON_H */
