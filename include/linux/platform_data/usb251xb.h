#ifndef __USB251XB_H__
#define __USB251XB_H__

struct usb251xb_platform_data {
	int gpio_reset;
	u16 vendor_id;
	u16 product_id;
	u16 device_id;
	u8 conf_data1;
	u8 conf_data2;
	u8 conf_data3;
	u8 non_rem_dev;
	u8 port_disable_sp;
	u8 port_disable_bp;
	u8 max_power_sp;
	u8 max_power_bp;
	u8 max_current_sp;
	u8 max_current_bp;
	u8 power_on_time;
	u16 lang_id;
	char manufacturer[31];	/* NULL terminated ASCII string */
	char product[31];	/* NULL terminated ASCII string */
	char serial[31];	/* NULL terminated ASCII string */
	u8 bat_charge_en;
	u8 boost_up;
	u8 boost_x;
	u8 port_swap;
	u8 port_map12;
	u8 port_map34;
	u8 status;
};

#endif
