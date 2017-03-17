/*
 * This file discribe the STM32 DFSDM IIO driver API for audio part
 *
 * Copyright (C) 2017, STMicroelectronics - All Rights Reserved
 * Author(s): Arnaud Pouliquen <arnaud.pouliquen@st.com>.
 *
 * License terms: GPL V2.0.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 */
#ifndef STM32_DFSDM_AUDIO_H
#define STM32_DFSDM_AUDIO_H

/**
 * stm32_dfsdm_get_buff_cb() - register callback for capture buffer period.
 * @dev:	Pointer to client device.
 * @indio_dev:	Device on which the channel exists.
 * @cb:		Callback function.
 *		@data:  pointer to data buffer
 *		@size: size of the data buffer in bytes
 * @private:	Private data passed to callback.
 *
 */
int stm32_dfsdm_get_buff_cb(struct iio_dev *iio_dev,
			    int (*cb)(const void *data, size_t size,
				      void *private),
			    void *private);
/**
 * stm32_dfsdm_get_buff_cb() - release callback for capture buffer period.
 * @indio_dev:	Device on which the channel exists.
 */
int stm32_dfsdm_release_buff_cb(struct iio_dev *iio_dev);

#endif
