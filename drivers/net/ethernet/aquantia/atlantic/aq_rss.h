/*
 * Aquantia Corporation Network Driver
 * Copyright (C) 2014-2016 Aquantia Corporation. All rights reserved
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

/*
 * File aq_rss.h: Receive Side Scaling definitions.
 */

#ifndef AQ_RSS_H
#define AQ_RSS_H

#include "aq_common.h"
#include "aq_cfg.h"

#define HASH_TYPE_IPV4                  0x00000100U
#define HASH_TYPE_TCP_IPV4              0x00000200U
#define HASH_TYPE_IPV6                  0x00000400U
#define HASH_TYPE_IPV6_EX               0x00000800U
#define HASH_TYPE_TCP_IPV6              0x00001000U
#define HASH_TYPE_TCP_IPV6_EX           0x00002000U

struct aq_receive_scale_parameters {
	u16 base_cpu_number;
	u16 indirection_table_size;
	u16 hash_secret_key_size;
	u32 hash_secret_key[AQ_CFG_RSS_HASHKEY_SIZE / sizeof(u32)];
	u8 indirection_table[AQ_CFG_RSS_INDIRECTION_TABLE_MAX];
};

#endif /* AQ_RSS_H */
