/*
 * mt2701-irq.h  --  Mediatek 2701 audio driver irq function definition
 *
 * Copyright (c) 2016 MediaTek Inc.
 * Author: Garlic Tseng <garlic.tseng@mediatek.com>
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

#ifndef _MT_2701_IRQ_H_
#define _MT_2701_IRQ_H_

u32 mt2701_asys_irq_status(struct mt2701_afe *afe);
void mt2701_asys_irq_clear(struct mt2701_afe *afe, u32 status);
void mt2701_memif_isr(struct mt2701_afe *afe, struct mt2701_afe_memif *memif);
int mt2701_asys_irq_acquire(struct mt2701_afe *afe);
int mt2701_asys_irq_release(struct mt2701_afe *afe, int irq_id);

#endif
