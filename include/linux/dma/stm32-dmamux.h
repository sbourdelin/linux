/*
 * stm32-dmamux.h
 *
 * Copyright (C) M'Boumba Cedric Madianga 2017
 * Author:  M'Boumba Cedric Madianga <cedric.madianga@gmail.com>
 * License terms:  GNU General Public License (GPL), version 2
 */

#ifndef __DMA_STM32_DMAMUX_H
#define __DMA_STM32_DMAMUX_H

#if defined(CONFIG_STM32_DMAMUX)
int stm32_dmamux_set_config(struct device *dev, void *route_data, u32 chan_id);
#else
int stm32_dmamux_set_config(struct device *dev, void *route_data, u32 chan_id)
{
	return -ENODEV;
}
#endif /* CONFIG_STM32_DMAMUX */

#endif /* __DMA_STM32_DMAMUX_H */
