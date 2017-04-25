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
#include "priv.h"

int
nvkm_therm_clkgate_engine(struct nvkm_therm *therm, enum nvkm_devidx subdev)
{
	if (!therm->func->clkgate_engine)
		return -1;

	return therm->func->clkgate_engine(subdev);
}

void
nvkm_therm_clkgate_set(struct nvkm_therm *therm, int gate_idx, bool enable)
{
	if (!therm->func->clkgate_set)
		return;

	if (enable)
		nvkm_trace(&therm->subdev,
			   "Enabling clockgating for gate 0x%x\n", gate_idx);
	else
		nvkm_trace(&therm->subdev,
			   "Disabling clockgating for gate 0x%x\n", gate_idx);

	therm->func->clkgate_set(therm, gate_idx, enable);
}
