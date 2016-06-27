/*
 * Copyright (c) 2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef __TEGRA_BPMP_H

typedef void (*bpmp_mrq_handler)(int mrq_code, void *data, int ch);

struct tegra_bpmp_ops {
	int (*send_receive)(int mrq_code, void *ob_data, int ob_sz,
			    void *ib_data, int ib_sz);
	int (*send_receive_atomic)(int mrq_code, void *ob_data, int ob_sz,
			    void *ib_data, int ib_sz);
	int (*request_mrq)(int mrq_code, bpmp_mrq_handler handler, void *data);
	void (*mrq_return)(int ch, int ret_code, int val);
};

struct tegra_bpmp_ops *tegra_bpmp_get_ops(void);

#endif /* __TEGRA_BPMP_H */
