/*
 * Copyright (c) 2017 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2017 Oleksandr Shamray <oleksandrs@mellanox.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __JTAG_H
#define __JTAG_H

#include <uapi/linux/jtag.h>

struct jtag;
/**
 * struct jtag_ops - callbacks for jtag control functions:
 *
 * @freq_get: get frequency function. Filled by device driver
 * @freq_set: set frequency function. Filled by device driver
 * @status_get: set status function. Filled by device driver
 * @idle: set JTAG to idle state function. Filled by device driver
 * @xfer: send JTAG xfer function. Filled by device driver
 */
struct jtag_ops {
	int (*freq_get)(struct jtag *jtag, unsigned long *freq);
	int (*freq_set)(struct jtag *jtag, unsigned long freq);
	int (*status_get)(struct jtag *jtag, enum jtag_endstate *state);
	int (*idle)(struct jtag *jtag, struct jtag_run_test_idle *idle);
	int (*xfer)(struct jtag *jtag, struct jtag_xfer *xfer);
};

void *jtag_priv(struct jtag *jtag);
int jtag_register(struct jtag *jtag);
void jtag_unregister(struct jtag *jtag);
struct jtag *jtag_alloc(size_t priv_size, const struct jtag_ops *ops);
void jtag_free(struct jtag *jtag);

#endif /* __JTAG_H */
