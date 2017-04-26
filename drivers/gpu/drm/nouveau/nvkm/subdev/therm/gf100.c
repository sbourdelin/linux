/*
 * Copyright 2017 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Lyude Paul
 */
#include <core/device.h>

#include "priv.h"

static inline int
gf100_clkgate_engine_offset(enum nvkm_devidx subdev)
{
	switch (subdev) {
		case NVKM_ENGINE_GR:     return 0x00;
		case NVKM_ENGINE_MSPDEC: return 0x04;
		case NVKM_ENGINE_MSPPP:  return 0x08;
		case NVKM_ENGINE_MSVLD:  return 0x0c;
		case NVKM_ENGINE_CE0:    return 0x10;
		case NVKM_ENGINE_CE1:    return 0x14;
		case NVKM_ENGINE_MSENC:  return 0x18;
		case NVKM_ENGINE_CE2:    return 0x1c;
		default:                 return -1;
	}
}

void
gf100_clkgate_engine(struct nvkm_therm *therm, enum nvkm_devidx subdev,
		     bool enable)
{
	int offset = gf100_clkgate_engine_offset(subdev);
	u8 data;

	if (offset == -1)
		return;

	if (enable) /* ENG_CLK=auto, BLK_CLK=auto, ENG_PWR=run, BLK_PWR=auto */
		data = 0x45;
	else        /* ENG_CLK=run, BLK_CLK=auto, ENG_PWR=run, BLK_PWR=auto*/
		data = 0x44;

	nvkm_mask(therm->subdev.device, 0x20200 + offset, 0xff, data);
}

static const struct nvkm_therm_func
gf100_therm = {
	.init = gt215_therm_init,
	.fini = g84_therm_fini,
	.pwm_ctrl = nv50_fan_pwm_ctrl,
	.pwm_get = nv50_fan_pwm_get,
	.pwm_set = nv50_fan_pwm_set,
	.pwm_clock = nv50_fan_pwm_clock,
	.temp_get = g84_temp_get,
	.fan_sense = gt215_therm_fan_sense,
	.program_alarms = nvkm_therm_program_alarms_polling,
	.clkgate_engine = gf100_clkgate_engine,
};

int
gf100_therm_new(struct nvkm_device *device, int index,
		struct nvkm_therm **ptherm)
{
	return nvkm_therm_new_(&gf100_therm, device, index, ptherm);
}
