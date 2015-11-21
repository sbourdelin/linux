#ifndef __HID_LOGITECH_HIDPP_FF_H
#define __HID_LOGITECH_HIDPP_FF_H

#include "hid-logitech-hidpp-base.h"

int hidpp_ff_init(struct hidpp_device *hidpp, u8 feature_index);
int hidpp_ff_deinit(struct hid_device *hid);

#endif
