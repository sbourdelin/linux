/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "i915_params.h"
#include "i915_drv.h"

struct i915_params i915 __read_mostly = {
#define MEMBER(Type, Name, Value, Mode, Unsafe, Brief, Detailed) .Name = Value,
	I915_PARAMS_FOR_EACH(MEMBER)
#undef MEMBER
};

#undef NULL /* we don't want to see (void*)0 in the description */
#define NULL ""
#define PARAM(Type, Name, Value, Mode, Unsafe, Brief, Detailed) \
	module_param_named##Unsafe(Name, i915.Name, Type, Mode); \
	MODULE_PARM_DESC(Name, Brief " " Detailed "[default: " __stringify(Value) "]");
I915_PARAMS_FOR_EACH(PARAM)
#undef PARAM

