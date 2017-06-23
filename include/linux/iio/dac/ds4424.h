/*
 * Maxim Integrated
 * 7-bit, Multi-Channel Sink/Source Current DAC Driver
 * Copyright (C) 2017 Maxim Integrated
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef IIO_DAC_DS4424_H_
#define IIO_DAC_DS4424_H_
#include <linux/iio/iio.h>
#include <linux/iio/machine.h>

#define DS4422_MAX_DAC_CHANNELS		2
#define DS4424_MAX_DAC_CHANNELS		4
#define DS442X_MAX_DAC_CHANNELS		DS4424_MAX_DAC_CHANNELS

struct ds4424_pdata {
	const char *vcc_supply_name;
	uint32_t max_rfs;
	uint32_t min_rfs;
	uint32_t ifs_scale;
	uint32_t max_picoamp;
	uint32_t rfs_res[DS442X_MAX_DAC_CHANNELS];
	struct iio_map dac_iio_map[DS442X_MAX_DAC_CHANNELS + 1];
};
#endif /* IIO_DAC_DS4424_H_ */
