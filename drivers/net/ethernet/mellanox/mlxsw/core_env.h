/*
 * drivers/net/ethernet/mellanox/mlxsw/core_env.h
 * Copyright (c) 2018 Mellanox Technologies. All rights reserved.
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

#ifndef _MLXSW_CORE_ENV_H
#define _MLXSW_CORE_ENV_H

#define MLXSW_ENV_TEMP_UNREACHABLE	150000	/* 150C */
#define MLXSW_ENV_HOT_MASK		BIT(0)
#define MLXSW_ENV_CRIT_MASK		BIT(1)
#define MLXSW_ENV_TEMP_NORM		75000	/* 75C */
#define MLXSW_ENV_TEMP_HIGH		85000	/* 85C */
#define MLXSW_ENV_TEMP_HOT		105000	/* 105C */
#define MLXSW_ENV_TEMP_CRIT		110000	/* 110C */
#define MLXSW_ENV_TEMP_WINDOW		(MLXSW_ENV_TEMP_HOT - \
					 MLXSW_ENV_TEMP_NORM)

struct mlxsw_env_temp_thresh {
	int normal;
	int hot;
	int crit;
};

struct mlxsw_env_temp_multi {
	struct mlxsw_env_temp_thresh thresh;
	u8 mask;
};

int mlxsw_env_collect_port_temp(struct mlxsw_core *core, int *ports_temp_cache,
				int port_count,
				struct mlxsw_env_temp_multi *multi,
				struct mlxsw_env_temp_thresh *delta,
				bool *untrusted_sensor, int *temp);
#endif
