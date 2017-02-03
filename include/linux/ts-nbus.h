/*
 * Copyright (c) 2016 - Savoir-faire Linux
 * Author: Sebastien Bourdelin <sebastien.bourdelin@savoirfairelinux.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef _TS_NBUS_H
#define _TS_NBUS_H

struct ts_nbus {
	struct pwm_device *pwm;
	struct gpio_descs *data;
	struct gpio_desc *csn;
	struct gpio_desc *txrx;
	struct gpio_desc *strobe;
	struct gpio_desc *ale;
	struct gpio_desc *rdy;
};

extern u16 ts_nbus_read(struct ts_nbus *, u8 adr);
extern int ts_nbus_write(struct ts_nbus *, u8 adr, u16 value);

#endif /* _TS_NBUS_H */
