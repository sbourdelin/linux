/*
 * stm32-timer-trigger.h
 *
 * Copyright (C) STMicroelectronics 2016
 * Author: Benjamin Gaignard <benjamin.gaignard@st.com> for STMicroelectronics.
 * License terms:  GNU General Public License (GPL), version 2
 */

#ifndef _STM32_TIMER_TRIGGER_H_
#define _STM32_TIMER_TRIGGER_H_

#include <dt-bindings/iio/timer/st,stm32-timer-triggers.h>

bool is_stm32_timer_trigger(struct iio_trigger *trig);

#endif
