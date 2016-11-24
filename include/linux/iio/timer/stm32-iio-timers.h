/*
 * stm32-iio-timers.h
 *
 * Copyright (C) STMicroelectronics 2016
 * Author: Benjamin Gaignard <benjamin.gaignard@st.com> for STMicroelectronics.
 * License terms:  GNU General Public License (GPL), version 2
 */

#ifndef _STM32_IIO_TIMERS_H_
#define _STM32_IIO_TIMERS_H_

#include <dt-bindings/iio/timer/st,stm32-iio-timer.h>

bool is_stm32_iio_timer_trigger(struct iio_trigger *trig);

#endif
