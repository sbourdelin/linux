/*
 * Driver for KeyStream wireless LAN cards.
 *
 * Copyright (C) 2005-2008 KeyStream Corp.
 * Copyright (C) 2009 Renesas Technology Corp.
 * Copyright (C) 2017 Tobin C. Harding.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef _KS7010_SDIO_H
#define _KS7010_SDIO_H

#include <linux/firmware.h>

#include "common.h"

int ks7010_sdio_tx(struct ks7010 *ks, u8 *data, size_t size);
bool ks7010_sdio_fw_is_running(struct ks7010 *ks);

u8 ks7010_sdio_read_trx_status_byte(struct ks7010 *ks);
bool ks7010_sdio_can_tx(struct ks7010 *ks, u8 trx_status_byte);

int ks7010_sdio_rx_read(struct ks7010 *ks, u8 *buf, size_t size);
void ks7010_sdio_set_read_status_idle(struct ks7010 *ks);

int ks7010_sdio_upload_fw(struct ks7010 *ks, u8 *fw, size_t fw_size);
bool ks7010_sdio_fw_is_running(struct ks7010 *ks);

#endif	/* _KS7010_SDIO_H */
