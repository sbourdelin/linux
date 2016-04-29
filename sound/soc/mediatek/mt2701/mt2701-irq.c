/*
 * mt2701-irq.c  --  Mediatek 2701 audio driver irq function
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

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>

#include "mt2701-afe-common.h"
#include "mt2701-irq.h"

u32 mt2701_asys_irq_status(struct mt2701_afe *afe)
{
	u32 status = 0;

	regmap_read(afe->regmap, ASYS_IRQ_STATUS, &status);
	return status;
}

void mt2701_asys_irq_clear(struct mt2701_afe *afe, u32 status)
{
	regmap_write(afe->regmap, ASYS_IRQ_CLR, status);
}

void mt2701_memif_isr(struct mt2701_afe *afe, struct mt2701_afe_memif *memif)
{
	if (memif)
		snd_pcm_period_elapsed(memif->substream);
}

static DEFINE_MUTEX(asys_irqs_lock);
int mt2701_asys_irq_acquire(struct mt2701_afe *afe)
{
	int i;

	mutex_lock(&asys_irqs_lock);
	for (i = MT2701_IRQ_ASYS_START; i < MT2701_IRQ_ASYS_END; ++i) {
		if (afe->irqs[i].irq_occupyed == 0) {
			afe->irqs[i].irq_occupyed = 1;
			mutex_unlock(&asys_irqs_lock);
			return i;
		}
	}
	mutex_unlock(&asys_irqs_lock);
	return MT2701_IRQ_ASYS_END;
}

int mt2701_asys_irq_release(struct mt2701_afe *afe, int irq_id)
{
	mutex_lock(&asys_irqs_lock);
	if (irq_id >= MT2701_IRQ_ASYS_START && irq_id < MT2701_IRQ_ASYS_END) {
		afe->irqs[irq_id].irq_occupyed = 0;
		mutex_unlock(&asys_irqs_lock);
		return 0;
	}
	mutex_unlock(&asys_irqs_lock);
	return -EINVAL;
}

MODULE_DESCRIPTION("MT2701 irq control");
MODULE_AUTHOR("Garlic Tseng <garlic.tseng@mediatek.com>");
MODULE_LICENSE("GPL v2");
