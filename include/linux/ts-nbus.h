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

extern u16 ts_nbus_read(u8 adr);
extern int ts_nbus_write(u8 adr, u16 value);
extern bool ts_nbus_is_ready(void);

#endif /* _TS_NBUS_H */
