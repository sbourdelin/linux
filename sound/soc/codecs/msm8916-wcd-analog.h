/*
 * msm8916-wcd-analog.h - MSM8916 analog audio codec interface
 *
 * Copyright 2017 - Savoir-faire Linux, Inc.
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */

#ifndef _MSM8916_WCD_ANALOG_H
#define _MSM8916_WCD_ANALOG_H

int pm8916_wcd_analog_jack_detect(struct snd_soc_codec *codec,
                                  struct snd_soc_jack *jack);

#endif
