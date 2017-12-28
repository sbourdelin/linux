// SPDX-License-Identifier: GPL-2.0+
#ifndef __ASM_RENESAS_CEU_H__
#define __ASM_RENESAS_CEU_H__

#define CEU_MAX_SUBDEVS		2

struct ceu_async_subdev {
	unsigned long flags;
	unsigned char bus_width;
	unsigned char bus_shift;
	unsigned int i2c_adapter_id;
	unsigned int i2c_address;
};

struct ceu_info {
	unsigned int num_subdevs;
	struct ceu_async_subdev subdevs[CEU_MAX_SUBDEVS];
};

#endif /* __ASM_RENESAS_CEU_H__ */
