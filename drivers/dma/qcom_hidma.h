/*
 * Qualcomm Technologies HIDMA data structures
 *
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef QCOM_HIDMA_H
#define QCOM_HIDMA_H

struct hidma_lldev;
struct hidma_llchan;
struct seq_file;
struct hidma_lldev;

int hidma_ll_request(struct hidma_lldev *llhndl, u32 dev_id,
			const char *dev_name,
			void (*callback)(void *data), void *data, u32 *tre_ch);

void hidma_ll_free(struct hidma_lldev *llhndl, u32 tre_ch);
enum dma_status hidma_ll_status(struct hidma_lldev *llhndl, u32 tre_ch);
bool hidma_ll_isenabled(struct hidma_lldev *llhndl);
int hidma_ll_queue_request(struct hidma_lldev *llhndl, u32 tre_ch);
int hidma_ll_start(struct hidma_lldev *llhndl);
int hidma_ll_pause(struct hidma_lldev *llhndl);
int hidma_ll_resume(struct hidma_lldev *llhndl);
void hidma_ll_set_transfer_params(struct hidma_lldev *llhndl, u32 tre_ch,
	dma_addr_t src, dma_addr_t dest, u32 len, u32 flags);
int hidma_ll_setup(struct hidma_lldev *lldev);
struct hidma_lldev *hidma_ll_init(struct device *dev, u32 max_channels,
			void __iomem *trca, void __iomem *evca,
			u8 evridx);
int hidma_ll_uninit(struct hidma_lldev *llhndl);
irqreturn_t hidma_ll_inthandler(int irq, void *arg);
void hidma_cleanup_pending_tre(struct hidma_lldev *llhndl, u8 err_info,
				u8 err_code);
#endif
