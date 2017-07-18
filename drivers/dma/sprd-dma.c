/*
 * Copyright (C) 2017 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/dma/sprd-dma.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include "dmaengine.h"

#define SPRD_DMA_DESCRIPTORS		16
#define SPRD_DMA_CFG_COUNT		32
#define SPRD_DMA_CHN_REG_OFFSET		0x1000
#define SPRD_DMA_CHN_REG_LENGTH		0x40
#define SPRD_DMA_MEMCPY_MIN_SIZE	64

/* DMA global registers definition */
#define DMA_GLB_PAUSE			0x0
#define DMA_GLB_FRAG_WAIT		0x4
#define DMA_GLB_REQ_PEND0_EN		0x8
#define DMA_GLB_REQ_PEND1_EN		0xc
#define DMA_GLB_INT_RAW_STS		0x10
#define DMA_GLB_INT_MSK_STS		0x14
#define DMA_GLB_REQ_STS			0x18
#define DMA_GLB_CHN_EN_STS		0x1c
#define DMA_GLB_DEBUG_STS		0x20
#define DMA_GLB_ARB_SEL_STS		0x24
#define DMA_GLB_CHN_START_CHN_CFG1	0x28
#define DMA_GLB_CHN_START_CHN_CFG2	0x2c
#define DMA_CHN_LLIST_OFFSET		0x10
#define DMA_GLB_REQ_CID(base, uid)	\
		((unsigned long)(base) + 0x2000 + 0x4 * ((uid) - 1))

/* DMA_GLB_CHN_START_CHN_CFG register definition */
#define SRC_CHN_OFFSET			0
#define DST_CHN_OFFSET			8
#define START_MODE_OFFSET		16
#define CHN_START_CHN			BIT(24)

/* DMA channel registers definition */
#define DMA_CHN_PAUSE			0x0
#define DMA_CHN_REQ			0x4
#define DMA_CHN_CFG			0x8
#define DMA_CHN_INTC			0xc
#define DMA_CHN_SRC_ADDR		0x10
#define DMA_CHN_DES_ADDR		0x14
#define DMA_CHN_FRG_LEN			0x18
#define DMA_CHN_BLK_LEN			0x1c
#define DMA_CHN_TRSC_LEN		0x20
#define DMA_CHN_TRSF_STEP		0x24
#define DMA_CHN_WARP_PTR		0x28
#define DMA_CHN_WARP_TO			0x2c
#define DMA_CHN_LLIST_PTR		0x30
#define DMA_CHN_FRAG_STEP		0x34
#define DMA_CHN_SRC_BLK_STEP		0x38
#define DMA_CHN_DES_BLK_STEP		0x3c

/* DMA_CHN_INTC register definition */
#define DMA_CHN_INT_MASK		GENMASK(4, 0)
#define DMA_CHN_INT_CLR_OFFSET		24
#define FRAG_INT_EN			BIT(0)
#define BLK_INT_EN			BIT(1)
#define TRANS_INT_EN			BIT(2)
#define LIST_INT_EN			BIT(3)
#define CFG_ERROR_INT_EN		BIT(4)

/* DMA_CHN_CFG register definition */
#define DMA_CHN_EN			BIT(0)
#define DMA_CHN_PRIORITY_OFFSET		12
#define LLIST_EN_OFFSET			4
#define CHN_WAIT_BDONE			24
#define DMA_DONOT_WAIT_BDONE		1

/* DMA_CHN_REQ register definition */
#define DMA_CHN_REQ_EN			BIT(0)

/* DMA_CHN_PAUSE register definition */
#define DMA_CHN_PAUSE_EN		BIT(0)
#define DMA_CHN_PAUSE_STS		BIT(2)
#define DMA_CHN_PAUSE_CNT		0x2000

/* DMA_CHN_WARP_* register definition */
#define DMA_CHN_ADDR_MASK		GENMASK(31, 28)
#define DMA_CHN_HIGH_ADDR_OFFSET	4
#define WRAP_DATA_MASK			GENMASK(27, 0)

/* DMA_CHN_INTC register definition */
#define FRAG_INT_STS			BIT(8)
#define BLK_INT_STS			BIT(9)
#define TRSC_INT_STS			BIT(10)
#define LLIST_INT_STS			BIT(11)
#define CFGERR_INT_STS			BIT(12)

/* DMA_CHN_FRG_LEN register definition */
#define SRC_DATAWIDTH_OFFSET		30
#define DES_DATAWIDTH_OFFSET		28
#define SWT_MODE_OFFSET			26
#define REQ_MODE_OFFSET			24
#define REQ_MODE_MASK			0x3
#define ADDR_WRAP_SEL_OFFSET		23
#define ADDR_WRAP_EN_OFFSET		22
#define ADDR_FIX_SEL_OFFSET		21
#define ADDR_FIX_SEL_EN			20
#define LLIST_END_OFFSET		19
#define BLK_LEN_REC_H_OFFSET		17
#define FRG_LEN_OFFSET			0
#define FRG_LEN_MASK			GENMASK(16, 0)

/* DMA_CHN_BLK_LEN register definition */
#define BLK_LEN_MASK			GENMASK(16, 0)

/* DMA_CHN_TRSC_LEN register definition */
#define TRSC_LEN_MASK			GENMASK(27, 0)

/* DMA_CHN_TRSF_STEP register definition */
#define DEST_TRSF_STEP_OFFSET		16
#define SRC_TRSF_STEP_OFFSET		0
#define TRSF_STEP_MASK			GENMASK(15, 0)

/* DMA_CHN_FRAG_STEP register definition */
#define DEST_FRAG_STEP_OFFSET		16
#define SRC_FRAG_STEP_OFFSET		0
#define FRAG_STEP_MASK			GENMASK(15, 0)

/* DMA_CHN_SRC_BLK_STEP register definition */
#define PTR_HIGH_ADDR_MASK		GENMASK(31, 28)
#define PTR_HIGH_ADDR_OFFSET		4

enum dma_config_type {
	SINGLE_CONFIG,
	LINKLIST_CONFIG,
};

/* dma channel configuration */
struct sprd_dma_chn_config {
	u32 pause;
	u32 req;
	u32 cfg;
	u32 intc;
	u32 src_addr;
	u32 des_addr;
	u32 frg_len;
	u32 blk_len;
	u32 trsc_len;
	u32 trsf_step;
	u32 wrap_ptr;
	u32 wrap_to;
	u32 llist_ptr;
	u32 frg_step;
	u32 src_blk_step;
	u32 des_blk_step;
};

/* dma request description */
struct sprd_dma_desc {
	struct dma_async_tx_descriptor	desc;
	struct sprd_dma_chn_config	*chn_config;
	struct list_head		node;
	enum dma_flags			dma_flags;
	int				cycle;
};

/* dma channel description */
struct sprd_dma_chn {
	struct dma_chan			chan;
	struct list_head		free;
	struct list_head		prepared;
	struct list_head		queued;
	struct list_head		active;
	struct list_head		completed;
	void __iomem			*chn_base;
	spinlock_t			chn_lock;
	int				chn_num;
	u32				dev_id;
	void				*dma_desc_configs;
	struct sprd_dma_cfg		dma_cfg[SPRD_DMA_CFG_COUNT];
	int				cfg_count;
};

/* SPRD dma device */
struct sprd_dma_dev {
	struct dma_device		dma_dev;
	void __iomem			*glb_base;
	struct clk			*clk;
	struct clk			*ashb_clk;
	int				irq;
	struct tasklet_struct		tasklet;
	struct kmem_cache		*dma_desc_node_cachep;
	u32				total_chns;
	struct sprd_dma_chn		channels[0];
};

static bool sprd_dma_filter_fn(struct dma_chan *chan, void *param);
static struct of_dma_filter_info sprd_dma_info = {
	.filter_fn = sprd_dma_filter_fn,
};

static inline struct sprd_dma_chn *to_sprd_dma_chan(struct dma_chan *c)
{
	return container_of(c, struct sprd_dma_chn, chan);
}

static inline struct sprd_dma_dev *to_sprd_dma_dev(struct dma_chan *c)
{
	struct sprd_dma_chn *mchan = to_sprd_dma_chan(c);

	return container_of(mchan, struct sprd_dma_dev, channels[c->chan_id]);
}

static inline struct sprd_dma_desc *
to_sprd_dma_desc(struct dma_async_tx_descriptor *tx)
{
	return container_of(tx, struct sprd_dma_desc, desc);
}

static int sprd_dma_enable(struct sprd_dma_dev *sdev)
{
	int ret;

	ret = clk_prepare_enable(sdev->clk);
	if (ret)
		return ret;

	if (!IS_ERR(sdev->ashb_clk))
		ret = clk_prepare_enable(sdev->ashb_clk);

	return ret;
}

static void sprd_dma_disable(struct sprd_dma_dev *sdev)
{
	clk_disable_unprepare(sdev->clk);

	if (!IS_ERR(sdev->ashb_clk))
		clk_disable_unprepare(sdev->ashb_clk);
}

static void sprd_dma_set_uid(struct sprd_dma_chn *mchan)
{
	struct sprd_dma_dev *sdev = to_sprd_dma_dev(&mchan->chan);
	u32 dev_id = mchan->dev_id;

	if (dev_id != DMA_SOFTWARE_UID)
		writel(mchan->chn_num + 1, (void __iomem *)DMA_GLB_REQ_CID(
		       sdev->glb_base, dev_id));
}

static void sprd_dma_unset_uid(struct sprd_dma_chn *mchan)
{
	struct sprd_dma_dev *sdev = to_sprd_dma_dev(&mchan->chan);
	u32 dev_id = mchan->dev_id;

	if (dev_id != DMA_SOFTWARE_UID)
		writel(0, (void __iomem *)DMA_GLB_REQ_CID(sdev->glb_base,
		       dev_id));
}

static void sprd_dma_clear_int(struct sprd_dma_chn *mchan)
{
	u32 intc = readl(mchan->chn_base + DMA_CHN_INTC);

	intc |= DMA_CHN_INT_MASK << DMA_CHN_INT_CLR_OFFSET;
	writel(intc, mchan->chn_base + DMA_CHN_INTC);
}

static void sprd_dma_enable_chn(struct sprd_dma_chn *mchan)
{
	u32 cfg = readl(mchan->chn_base + DMA_CHN_CFG);

	cfg |= DMA_CHN_EN;
	writel(cfg, mchan->chn_base + DMA_CHN_CFG);
}

static void sprd_dma_disable_chn(struct sprd_dma_chn *mchan)
{
	u32 cfg = readl(mchan->chn_base + DMA_CHN_CFG);

	cfg &= ~DMA_CHN_EN;
	writel(cfg, mchan->chn_base + DMA_CHN_CFG);
}

static void sprd_dma_soft_request(struct sprd_dma_chn *mchan)
{
	u32 req = readl(mchan->chn_base + DMA_CHN_REQ);

	req |= DMA_CHN_REQ_EN;
	writel(req, mchan->chn_base + DMA_CHN_REQ);
}

static void sprd_dma_stop_and_disable(struct sprd_dma_chn *mchan)
{
	u32 cfg = readl(mchan->chn_base + DMA_CHN_CFG);
	u32 pause, timeout = DMA_CHN_PAUSE_CNT;

	if (!(cfg & DMA_CHN_EN))
		return;

	pause = readl(mchan->chn_base + DMA_CHN_PAUSE);
	pause |= DMA_CHN_PAUSE_EN;
	writel(pause, mchan->chn_base + DMA_CHN_PAUSE);

	do {
		pause = readl(mchan->chn_base + DMA_CHN_PAUSE);
		if (pause & DMA_CHN_PAUSE_STS)
			break;

		cpu_relax();
	} while (--timeout > 0);

	sprd_dma_disable_chn(mchan);
	writel(0, mchan->chn_base + DMA_CHN_PAUSE);
}

static unsigned long sprd_dma_get_dst_addr(struct sprd_dma_chn *mchan)
{
	unsigned long addr;

	addr = readl(mchan->chn_base + DMA_CHN_DES_ADDR);
	addr |= (readl(mchan->chn_base + DMA_CHN_WARP_TO) &
		 DMA_CHN_ADDR_MASK) << DMA_CHN_HIGH_ADDR_OFFSET;
	return addr;
}

static enum dma_int_type sprd_dma_get_int_type(struct sprd_dma_chn *mchan)
{
	u32 intc_reg = readl(mchan->chn_base + DMA_CHN_INTC);

	if (intc_reg & CFGERR_INT_STS)
		return CONFIG_ERR;
	else if (intc_reg & LLIST_INT_STS)
		return LIST_DONE;
	else if (intc_reg & TRSC_INT_STS)
		return TRANS_DONE;
	else if (intc_reg & BLK_INT_STS)
		return BLK_DONE;
	else if (intc_reg & FRAG_INT_STS)
		return FRAG_DONE;
	else
		return NO_INT;
}

static enum dma_request_mode sprd_dma_get_req_type(struct sprd_dma_chn *mchan)
{
	u32 frag_reg = readl(mchan->chn_base + DMA_CHN_FRG_LEN);

	switch ((frag_reg >> REQ_MODE_OFFSET) & REQ_MODE_MASK) {
	case 0:
		return FRAG_REQ_MODE;
	case 1:
		return BLOCK_REQ_MODE;
	case 2:
		return TRANS_REQ_MODE;
	case 3:
		return LIST_REQ_MODE;
	default:
		return FRAG_REQ_MODE;
	}
}

static int sprd_dma_chn_start_chn(struct sprd_dma_chn *mchan,
				  struct sprd_dma_desc *mdesc)
{
	struct sprd_dma_dev *sdev = to_sprd_dma_dev(&mchan->chan);
	enum dma_flags flag = mdesc->dma_flags;
	int chn = mchan->chn_num + 1;
	unsigned int cfg_group1, cfg_group2, start_mode = 0;

	if (!(flag & (DMA_GROUP1_SRC | DMA_GROUP2_SRC | DMA_GROUP1_DST |
		      DMA_GROUP2_DST)))
		return 0;

	if (flag & (DMA_GROUP1_SRC | DMA_GROUP2_SRC)) {
		switch (flag & (DMA_MUTL_FRAG_DONE | DMA_MUTL_BLK_DONE |
			DMA_MUTL_TRANS_DONE | DMA_MUTL_LIST_DONE)) {
		case DMA_MUTL_FRAG_DONE:
			start_mode = 0;
			break;
		case DMA_MUTL_BLK_DONE:
			start_mode = 1;
			break;
		case DMA_MUTL_TRANS_DONE:
			start_mode = 2;
			break;
		case DMA_MUTL_LIST_DONE:
			start_mode = 3;
			break;
		default:
			dev_err(sdev->dma_dev.dev,
				"chn stat chn mode incorrect\n");
			return -EINVAL;
		}
	}

	cfg_group1 = readl(sdev->glb_base + DMA_GLB_CHN_START_CHN_CFG1);
	cfg_group2 = readl(sdev->glb_base + DMA_GLB_CHN_START_CHN_CFG2);

	switch (flag & (DMA_GROUP1_SRC | DMA_GROUP2_SRC |
		DMA_GROUP1_DST | DMA_GROUP2_DST)) {
	case DMA_GROUP1_SRC:
		cfg_group1 |= chn << SRC_CHN_OFFSET;
		cfg_group1 |= (1 << start_mode) << START_MODE_OFFSET;
		cfg_group1 |= CHN_START_CHN;
		writel(cfg_group1, sdev->glb_base + DMA_GLB_CHN_START_CHN_CFG1);
		break;
	case DMA_GROUP2_SRC:
		cfg_group2 |= chn << SRC_CHN_OFFSET;
		cfg_group2 |= (1 << start_mode) << START_MODE_OFFSET;
		cfg_group2 |= CHN_START_CHN;
		writel(cfg_group2, sdev->glb_base + DMA_GLB_CHN_START_CHN_CFG2);
		break;
	case DMA_GROUP1_DST:
		cfg_group1 |= chn << DST_CHN_OFFSET;
		cfg_group1 |= CHN_START_CHN;
		writel(cfg_group1, sdev->glb_base + DMA_GLB_CHN_START_CHN_CFG1);
		break;
	case DMA_GROUP2_DST:
		cfg_group2 |= chn << DST_CHN_OFFSET;
		cfg_group2 |= CHN_START_CHN;
		writel(cfg_group2, sdev->glb_base + DMA_GLB_CHN_START_CHN_CFG2);
		break;
	default:
		break;
	}

	return 0;
}

static int sprd_dma_set_chn_config(struct sprd_dma_chn *mchan,
				   struct sprd_dma_desc *mdesc)
{
	struct sprd_dma_chn_config *cfg = mdesc->chn_config;
	int ret;

	ret = sprd_dma_chn_start_chn(mchan, mdesc);
	if (ret)
		return ret;

	writel(cfg->pause, mchan->chn_base + DMA_CHN_PAUSE);
	writel(cfg->cfg, mchan->chn_base + DMA_CHN_CFG);
	writel(cfg->intc, mchan->chn_base + DMA_CHN_INTC);
	writel(cfg->src_addr, mchan->chn_base + DMA_CHN_SRC_ADDR);
	writel(cfg->des_addr, mchan->chn_base + DMA_CHN_DES_ADDR);
	writel(cfg->frg_len, mchan->chn_base + DMA_CHN_FRG_LEN);
	writel(cfg->blk_len, mchan->chn_base + DMA_CHN_BLK_LEN);
	writel(cfg->trsc_len, mchan->chn_base + DMA_CHN_TRSC_LEN);
	writel(cfg->trsf_step, mchan->chn_base + DMA_CHN_TRSF_STEP);
	writel(cfg->wrap_ptr, mchan->chn_base + DMA_CHN_WARP_PTR);
	writel(cfg->wrap_to, mchan->chn_base + DMA_CHN_WARP_TO);
	writel(cfg->llist_ptr, mchan->chn_base + DMA_CHN_LLIST_PTR);
	writel(cfg->frg_step, mchan->chn_base + DMA_CHN_FRAG_STEP);
	writel(cfg->src_blk_step, mchan->chn_base + DMA_CHN_SRC_BLK_STEP);
	writel(cfg->des_blk_step, mchan->chn_base + DMA_CHN_DES_BLK_STEP);
	writel(cfg->req, mchan->chn_base + DMA_CHN_REQ);

	return 0;
}

static int sprd_dma_start(struct sprd_dma_chn *mchan)
{
	struct sprd_dma_desc *first;
	int ret;

	if (list_empty(&mchan->active))
		return 0;

	/*
	 * Get the first DMA descriptor from active list, and copy the DMA
	 * configuration from DMA descriptor to this DMA channel.
	 */
	first = list_first_entry(&mchan->active, struct sprd_dma_desc, node);
	ret = sprd_dma_set_chn_config(mchan, first);
	if (ret)
		return ret;

	sprd_dma_set_uid(mchan);
	sprd_dma_enable_chn(mchan);

	if (mchan->dev_id == DMA_SOFTWARE_UID &&
	    !(first->dma_flags & (DMA_GROUP1_DST | DMA_GROUP2_DST)))
		sprd_dma_soft_request(mchan);

	return 0;
}

static void sprd_dma_stop(struct sprd_dma_chn *mchan)
{
	sprd_dma_unset_uid(mchan);
	sprd_dma_stop_and_disable(mchan);
	sprd_dma_clear_int(mchan);
}

static int sprd_dma_process_completed(struct sprd_dma_dev *sdev)
{
	struct sprd_dma_chn *mchan;
	struct sprd_dma_desc *mdesc;
	struct sprd_dma_desc *first;
	struct dma_async_tx_descriptor *desc;
	u32 dma_total_chns = sdev->total_chns;
	dma_cookie_t last_cookie = 0;
	unsigned long flags;
	LIST_HEAD(list);
	int i;

	for (i = 0; i < dma_total_chns; i++) {
		mchan = &sdev->channels[i];

		spin_lock_irqsave(&mchan->chn_lock, flags);
		if (!list_empty(&mchan->completed))
			list_splice_tail_init(&mchan->completed, &list);
		spin_unlock_irqrestore(&mchan->chn_lock, flags);

		if (list_empty(&list))
			continue;

		list_for_each_entry(mdesc, &list, node) {
			desc = &mdesc->desc;

			if (desc->callback)
				desc->callback(desc->callback_param);

			dma_run_dependencies(desc);
			last_cookie = desc->cookie;
		}

		spin_lock_irqsave(&mchan->chn_lock, flags);
		list_splice_tail_init(&list, &mchan->free);

		/*
		 * Check if there are pending DMA descriptor in queued list
		 * need to be queued.
		 */
		if (!list_empty(&mchan->queued)) {
			list_for_each_entry(mdesc, &mchan->queued, node) {
				/*
				 * If the DMA descriptor flag is set
				 * DMA_GROUP1_DST or DMA_GROUP1_DST flag, which
				 * means this DMA descriptor do not need to be
				 * activated and it will be invoked by another
				 * DMA descriptor which set DMA_GROUP1_SRC or
				 * DMA_GROUP2_SRC flag.
				 */
				if (!(mdesc->dma_flags &
				      (DMA_GROUP1_DST | DMA_GROUP2_DST)) &&
				    list_empty(&mchan->active)) {
					first = list_first_entry(&mchan->queued,
						struct sprd_dma_desc, node);
					list_move_tail(&first->node,
						       &mchan->active);
					sprd_dma_start(mchan);
					break;
				}
			}
		} else {
			mchan->chan.completed_cookie = last_cookie;
		}

		spin_unlock_irqrestore(&mchan->chn_lock, flags);
	}

	return 0;
}

static void sprd_dma_tasklet(unsigned long data)
{
	struct sprd_dma_dev *sdev = (void *)data;

	sprd_dma_process_completed(sdev);
}

static bool sprd_dma_check_trans_done(struct sprd_dma_desc *mdesc,
				      enum dma_int_type int_type,
				      enum dma_request_mode req_mode)
{
	if (mdesc->cycle == 1)
		return false;

	if ((unsigned int)int_type >= ((unsigned int)req_mode + 1))
		return true;
	else
		return false;
}

static irqreturn_t dma_irq_handle(int irq, void *dev_id)
{
	struct sprd_dma_dev *sdev = (struct sprd_dma_dev *)dev_id;
	u32 irq_status = readl(sdev->glb_base + DMA_GLB_INT_MSK_STS);
	struct sprd_dma_chn *mchan;
	struct sprd_dma_desc *mdesc;
	struct dma_async_tx_descriptor *desc;
	enum dma_request_mode req_type;
	enum dma_int_type int_type;
	bool trans_done = false;
	u32 i;

	while (irq_status) {
		i = __ffs(irq_status);
		irq_status &= (irq_status - 1);
		mchan = &sdev->channels[i];

		spin_lock(&mchan->chn_lock);
		int_type = sprd_dma_get_int_type(mchan);
		req_type = sprd_dma_get_req_type(mchan);
		sprd_dma_clear_int(mchan);

		if (!list_empty(&mchan->active)) {
			mdesc = list_first_entry(&mchan->active,
						 struct sprd_dma_desc, node);
			/*
			 * Check if the dma request descriptor is done, if it
			 * is done we should move this descriptor into
			 * complete list to handle.
			 */
			trans_done = sprd_dma_check_trans_done(mdesc, int_type,
							       req_type);
			if (trans_done == true)
				list_splice_tail_init(&mchan->active,
						      &mchan->completed);
			/*
			 * If the cycle is 1, which means this DMA descriptor
			 * will always in active state until user free this DMA
			 * channel's resources. But user may need to be notified
			 * when transfer interrupt is generated.
			 */
			if (mdesc->cycle == 1) {
				desc = &mdesc->desc;
				spin_unlock(&mchan->chn_lock);
				if (desc->callback)
					desc->callback(desc->callback_param);
				continue;
			}
		}
		spin_unlock(&mchan->chn_lock);
	}

	tasklet_schedule(&sdev->tasklet);
	return IRQ_HANDLED;
}

static dma_cookie_t sprd_desc_submit(struct dma_async_tx_descriptor *tx)
{
	struct sprd_dma_chn *mchan = to_sprd_dma_chan(tx->chan);
	struct sprd_dma_desc *mdesc = to_sprd_dma_desc(tx);
	struct sprd_dma_desc *first;
	unsigned long flags;
	dma_cookie_t cookie;
	int ret;

	spin_lock_irqsave(&mchan->chn_lock, flags);
	cookie = dma_cookie_assign(tx);
	list_move_tail(&mdesc->node, &mchan->queued);

	/*
	 * If we set DMA_GROUP1_DST or DMA_GROUP2_DST flags, which means we do
	 * not need start this DMA transfer and it will be invoked by other DMA
	 * transfer setting DMA_GROUP1_SRC or DMA_GROUP2_SRC flag.
	 */
	if (mdesc->dma_flags & (DMA_GROUP1_DST | DMA_GROUP2_DST)) {
		ret = sprd_dma_set_chn_config(mchan, mdesc);
		if (ret) {
			spin_unlock_irqrestore(&mchan->chn_lock, flags);
			return ret;
		}

		sprd_dma_enable_chn(mchan);
	} else if (list_empty(&mchan->active)) {
		first = list_first_entry(&mchan->queued, struct sprd_dma_desc,
					 node);
		list_move_tail(&first->node, &mchan->active);
		ret = sprd_dma_start(mchan);
		if (ret)
			cookie = ret;
	}
	spin_unlock_irqrestore(&mchan->chn_lock, flags);

	return cookie;
}

static int sprd_dma_alloc_chan_resources(struct dma_chan *chan)
{
	struct sprd_dma_chn *mchan = to_sprd_dma_chan(chan);
	struct sprd_dma_dev *sdev = to_sprd_dma_dev(chan);
	struct sprd_dma_desc *mdesc, *tmp;
	struct sprd_dma_chn_config *chn_configs;
	LIST_HEAD(descs);
	unsigned long flags;
	int ret, i;

	ret = pm_runtime_get_sync(chan->device->dev);
	if (ret < 0) {
		dev_err(sdev->dma_dev.dev, "pm runtime get failed: %d\n", ret);
		return ret;
	}

	chn_configs = devm_kzalloc(sdev->dma_dev.dev,
			       SPRD_DMA_DESCRIPTORS *
			       sizeof(struct sprd_dma_chn_config),
			       GFP_KERNEL);
	if (!chn_configs) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	for (i = 0; i < SPRD_DMA_DESCRIPTORS; i++) {
		mdesc = (struct sprd_dma_desc *)kmem_cache_zalloc(
					sdev->dma_desc_node_cachep,
					GFP_ATOMIC);
		if (!mdesc) {
			ret = -ENOMEM;
			goto err_cache_alloc;
		}

		dma_async_tx_descriptor_init(&mdesc->desc, chan);
		mdesc->desc.flags = DMA_CTRL_ACK;
		mdesc->desc.tx_submit = sprd_desc_submit;
		mdesc->chn_config = &chn_configs[i];
		mdesc->cycle = 0;
		mdesc->dma_flags = 0;
		INIT_LIST_HEAD(&mdesc->node);
		list_add_tail(&mdesc->node, &descs);
	}

	spin_lock_irqsave(&mchan->chn_lock, flags);
	list_splice_tail_init(&descs, &mchan->free);
	spin_unlock_irqrestore(&mchan->chn_lock, flags);

	mchan->dma_desc_configs = chn_configs;
	mchan->dev_id = 0;
	mchan->cfg_count = 0;
	memset(mchan->dma_cfg, 0, sizeof(struct sprd_dma_cfg) *
	       SPRD_DMA_CFG_COUNT);

	return 0;

err_cache_alloc:
	list_for_each_entry_safe(mdesc, tmp, &descs, node)
		kmem_cache_free(sdev->dma_desc_node_cachep, mdesc);
	devm_kfree(sdev->dma_dev.dev, chn_configs);
err_alloc:
	pm_runtime_put_sync(chan->device->dev);
	return ret;
}

static void sprd_dma_free_chan_resources(struct dma_chan *chan)
{
	struct sprd_dma_chn *mchan = to_sprd_dma_chan(chan);
	struct sprd_dma_dev *sdev = to_sprd_dma_dev(chan);
	struct sprd_dma_desc *mdesc, *tmp;
	unsigned long flags;
	LIST_HEAD(descs);

	spin_lock_irqsave(&mchan->chn_lock, flags);
	list_splice_tail_init(&mchan->prepared, &mchan->free);
	list_splice_tail_init(&mchan->queued, &mchan->free);
	list_splice_tail_init(&mchan->active, &mchan->free);
	list_splice_tail_init(&mchan->completed, &mchan->free);
	list_splice_tail_init(&mchan->free, &descs);
	spin_unlock_irqrestore(&mchan->chn_lock, flags);

	list_for_each_entry_safe(mdesc, tmp, &descs, node)
		kmem_cache_free(sdev->dma_desc_node_cachep, mdesc);
	devm_kfree(sdev->dma_dev.dev, mchan->dma_desc_configs);

	spin_lock_irqsave(&mchan->chn_lock, flags);
	sprd_dma_stop(mchan);
	spin_unlock_irqrestore(&mchan->chn_lock, flags);

	pm_runtime_put_sync(chan->device->dev);
}

static enum dma_status sprd_dma_tx_status(struct dma_chan *chan,
					  dma_cookie_t cookie,
					  struct dma_tx_state *txstate)
{
	struct sprd_dma_chn *mchan = to_sprd_dma_chan(chan);
	unsigned long flags;
	enum dma_status ret;

	ret = dma_cookie_status(chan, cookie, txstate);

	spin_lock_irqsave(&mchan->chn_lock, flags);
	txstate->residue = sprd_dma_get_dst_addr(mchan);
	spin_unlock_irqrestore(&mchan->chn_lock, flags);

	return ret;
}

static void sprd_dma_issue_pending(struct dma_chan *chan)
{
	/*
	 * We are posting descriptors to the hardware as soon as
	 * they are ready, so this function does nothing.
	 */
}

static int sprd_dma_config(struct dma_chan *chan, struct sprd_dma_desc *mdesc,
			   struct sprd_dma_cfg *cfg_list,
			   struct sprd_dma_chn_config *chn_config,
			   enum dma_config_type type)
{
	struct sprd_dma_chn *mchan = to_sprd_dma_chan(chan);
	struct sprd_dma_dev *sdev = to_sprd_dma_dev(&mchan->chan);
	struct sprd_dma_cfg *cfg = cfg_list;
	struct sprd_dma_chn_config *dma_chn_config;
	static unsigned long first_link_p;
	unsigned long linklist_ptr = 0;
	u32 fix_mode = 0, fix_en = 0;
	u32 wrap_en = 0, wrap_mode = 0;
	u32 llist_en = 0, list_end = 0;

	if (!IS_ALIGNED(cfg->src_step, 1 << cfg->datawidth)) {
		dev_err(sdev->dma_dev.dev, "source step is not aligned\n");
		return -EINVAL;
	}

	if (!IS_ALIGNED(cfg->des_step, 1 << cfg->datawidth)) {
		dev_err(sdev->dma_dev.dev, "destination step is not aligned\n");
		return -EINVAL;
	}

	if (type == SINGLE_CONFIG)
		dma_chn_config = mdesc->chn_config;
	else if (type == LINKLIST_CONFIG)
		dma_chn_config = chn_config;
	else {
		dev_err(sdev->dma_dev.dev,
			"check configuration type failed %d\n", type);
		return -EINVAL;
	}

	if (!mchan->dev_id)
		mchan->dev_id = cfg->dev_id;

	dma_chn_config->pause = 0;
	dma_chn_config->req = 0;

	if (cfg->link_cfg_p)
		llist_en = 1;

	dma_chn_config->cfg = cfg->chn_pri << DMA_CHN_PRIORITY_OFFSET |
	    llist_en << LLIST_EN_OFFSET |
	    DMA_DONOT_WAIT_BDONE << CHN_WAIT_BDONE;

	if (cfg->wrap_ptr && cfg->wrap_to) {
		wrap_en = 1;
		dma_chn_config->wrap_ptr = cfg->wrap_ptr & WRAP_DATA_MASK;
		dma_chn_config->wrap_to = cfg->wrap_to & WRAP_DATA_MASK;
	}

	dma_chn_config->wrap_ptr |=
		(u32)((cfg->src_addr >> DMA_CHN_HIGH_ADDR_OFFSET) &
		      DMA_CHN_ADDR_MASK);
	dma_chn_config->wrap_to |=
		(u32)((cfg->des_addr >> DMA_CHN_HIGH_ADDR_OFFSET) &
		      DMA_CHN_ADDR_MASK);

	dma_chn_config->src_addr = (u32)cfg->src_addr;
	dma_chn_config->des_addr = (u32)cfg->des_addr;

	if ((cfg->src_step != 0 && cfg->des_step != 0) ||
	    ((cfg->src_step | cfg->des_step) == 0)) {
		fix_en = 0;
	} else {
		fix_en = 1;
		if (cfg->src_step)
			fix_mode = 1;
		else
			fix_mode = 0;
	}

	if (wrap_en == 1) {
		if (cfg->wrap_to == cfg->src_addr) {
			wrap_mode = 0;
		} else if (cfg->wrap_to == cfg->des_addr) {
			wrap_mode = 1;
		} else {
			dev_err(sdev->dma_dev.dev,
				"check wrap config failed\n");
			return -EINVAL;
		}
	}

	if (llist_en == 1 && cfg->is_end == DMA_END)
		list_end = 1;

	dma_chn_config->frg_len = (cfg->datawidth << SRC_DATAWIDTH_OFFSET) |
		(cfg->datawidth << DES_DATAWIDTH_OFFSET) |
		(cfg->swt_mode << SWT_MODE_OFFSET) |
		(cfg->req_mode << REQ_MODE_OFFSET) |
		(wrap_mode << ADDR_WRAP_SEL_OFFSET) |
		(wrap_en << ADDR_WRAP_EN_OFFSET) |
		(fix_mode << ADDR_FIX_SEL_OFFSET) |
		(fix_en << ADDR_FIX_SEL_EN) |
		(list_end << LLIST_END_OFFSET) |
		(cfg->fragmens_len & FRG_LEN_MASK);

	dma_chn_config->blk_len = cfg->block_len & BLK_LEN_MASK;

	if (type == SINGLE_CONFIG) {
		dma_chn_config->intc &= ~DMA_CHN_INT_MASK;
		dma_chn_config->intc |= CFG_ERROR_INT_EN;

		switch (cfg->irq_mode) {
		case NO_INT:
			break;
		case FRAG_DONE:
			dma_chn_config->intc |= FRAG_INT_EN;
			break;
		case BLK_DONE:
			dma_chn_config->intc |= BLK_INT_EN;
			break;
		case BLOCK_FRAG_DONE:
			dma_chn_config->intc |= BLK_INT_EN | FRAG_INT_EN;
			break;
		case TRANS_DONE:
			dma_chn_config->intc |= TRANS_INT_EN;
			break;
		case TRANS_FRAG_DONE:
			dma_chn_config->intc |= TRANS_INT_EN | FRAG_INT_EN;
			break;
		case TRANS_BLOCK_DONE:
			dma_chn_config->intc |= TRANS_INT_EN | BLK_INT_EN;
			break;
		case LIST_DONE:
			dma_chn_config->intc |= LIST_INT_EN;
			break;
		case CONFIG_ERR:
			dma_chn_config->intc |= CFG_ERROR_INT_EN;
			break;
		default:
			dev_err(sdev->dma_dev.dev, "irq mode failed\n");
			return -EINVAL;
		}
	} else {
		dma_chn_config->intc = 0;
	}

	if (cfg->transcation_len == 0)
		dma_chn_config->trsc_len = cfg->block_len & TRSC_LEN_MASK;
	else
		dma_chn_config->trsc_len = cfg->transcation_len &
			TRSC_LEN_MASK;

	dma_chn_config->trsf_step = (cfg->des_step & TRSF_STEP_MASK) <<
		DEST_TRSF_STEP_OFFSET |
		(cfg->src_step & TRSF_STEP_MASK) << SRC_TRSF_STEP_OFFSET;

	dma_chn_config->frg_step = (cfg->dst_frag_step & FRAG_STEP_MASK)
		<< DEST_FRAG_STEP_OFFSET |
		(cfg->src_frag_step & FRAG_STEP_MASK)
		<< SRC_FRAG_STEP_OFFSET;

	dma_chn_config->src_blk_step = cfg->src_blk_step;
	dma_chn_config->des_blk_step = cfg->dst_blk_step;

	if (first_link_p == 0)
		first_link_p = cfg->link_cfg_p;

	if (type == SINGLE_CONFIG) {
		linklist_ptr = first_link_p + DMA_CHN_LLIST_OFFSET;
		first_link_p = 0;
	} else if (type == LINKLIST_CONFIG) {
		if (cfg->is_end == DMA_LINK)
			linklist_ptr = first_link_p + DMA_CHN_LLIST_OFFSET;
		else
			linklist_ptr = cfg->link_cfg_p +
				sizeof(struct sprd_dma_chn_config) +
				DMA_CHN_LLIST_OFFSET;
	}

	dma_chn_config->src_blk_step |=
		(u32)((linklist_ptr >> PTR_HIGH_ADDR_OFFSET) &
		      PTR_HIGH_ADDR_MASK);
	dma_chn_config->llist_ptr = (u32)linklist_ptr;
	return 0;
}

static int sprd_dma_config_linklist(struct dma_chan *chan,
				    struct sprd_dma_desc *mdesc,
				    struct sprd_dma_cfg *cfg_list,
				    u32 node_size)
{
	struct sprd_dma_dev *sdev = to_sprd_dma_dev(chan);
	struct sprd_dma_chn_config *chn_config_list_v;
	struct sprd_dma_cfg start_list;
	int ret, i;

	chn_config_list_v = (struct sprd_dma_chn_config *)cfg_list[0].link_cfg_v;

	for (i = 0; i < node_size; i++) {
		if (cfg_list[i].link_cfg_v == 0 || cfg_list[i].link_cfg_p == 0) {
			dev_err(sdev->dma_dev.dev,
				"no allocated memory for list node\n");
			return -EINVAL;
		}

		ret = sprd_dma_config(chan, NULL, cfg_list + i,
				      chn_config_list_v + i,
				      LINKLIST_CONFIG);
		if (ret < 0) {
			dev_err(sdev->dma_dev.dev,
				"linklist configuration failed\n");
			return ret;
		}
	}

	memset((void *)&start_list, 0, sizeof(struct sprd_dma_cfg));
	start_list.link_cfg_p = cfg_list[0].link_cfg_p;
	start_list.irq_mode = cfg_list[0].irq_mode;
	start_list.src_addr = cfg_list[0].src_addr;
	start_list.des_addr = cfg_list[0].des_addr;

	if (cfg_list[node_size - 1].is_end == DMA_LINK)
		mdesc->cycle = 1;

	return sprd_dma_config(chan, mdesc, &start_list, NULL, SINGLE_CONFIG);
}

struct dma_async_tx_descriptor *sprd_dma_prep_dma_memcpy(struct dma_chan *chan,
							 dma_addr_t dest,
							 dma_addr_t src,
							 size_t len,
							 unsigned long flags)
{
	struct sprd_dma_dev *sdev = to_sprd_dma_dev(chan);
	struct sprd_dma_chn *mchan = to_sprd_dma_chan(chan);
	struct sprd_dma_desc *mdesc = NULL;
	struct sprd_dma_cfg dma_cfg;
	u32 datawidth, src_step, des_step;
	int cfg_count = mchan->cfg_count;
	unsigned long irq_flags;
	int ret;

	/*
	 * If user did not have configuration for DMA transfer, then we should
	 * configure the DMA controller as default configuration.
	 */
	if (cfg_count == 0) {
		if (IS_ALIGNED(len, 4)) {
			datawidth = 2;
			src_step = 4;
			des_step = 4;
		} else if (IS_ALIGNED(len, 2)) {
			datawidth = 1;
			src_step = 2;
			des_step = 2;
		} else {
			datawidth = 0;
			src_step = 1;
			des_step = 1;
		}

		memset(&dma_cfg, 0, sizeof(struct sprd_dma_cfg));
		dma_cfg.src_addr = src;
		dma_cfg.des_addr = dest;
		dma_cfg.datawidth = datawidth;
		dma_cfg.src_step = src_step;
		dma_cfg.des_step = src_step;
		dma_cfg.fragmens_len = SPRD_DMA_MEMCPY_MIN_SIZE;
		if (len <= BLK_LEN_MASK) {
			dma_cfg.block_len = len;
			dma_cfg.req_mode = BLOCK_REQ_MODE;
			dma_cfg.irq_mode = BLK_DONE;
		} else {
			dma_cfg.block_len = SPRD_DMA_MEMCPY_MIN_SIZE;
			dma_cfg.transcation_len = len;
			dma_cfg.req_mode = TRANS_REQ_MODE;
			dma_cfg.irq_mode = TRANS_DONE;
		}
		cfg_count = 1;
	} else if (cfg_count == 1) {
		memcpy(&dma_cfg, mchan->dma_cfg, sizeof(struct sprd_dma_cfg));
	}

	/* get one free DMA descriptor for this DMA transfer. */
	spin_lock_irqsave(&mchan->chn_lock, irq_flags);
	if (!list_empty(&mchan->free)) {
		mdesc = list_first_entry(&mchan->free,
					 struct sprd_dma_desc, node);
		list_del(&mdesc->node);
	}
	spin_unlock_irqrestore(&mchan->chn_lock, irq_flags);

	if (!mdesc) {
		dev_err(sdev->dma_dev.dev, "get free descriptor failed\n");
		sprd_dma_process_completed(sdev);
		goto out;
	}

	mdesc->dma_flags = flags;

	/*
	 * Set the DMA configuration into channel register of this DMA
	 * descriptor. If we have multiple configuration, which means
	 * need enable the link-list function, then we should set these
	 * configuration into link-list address.
	 */
	if (cfg_count == 1)
		ret = sprd_dma_config(chan, mdesc, &dma_cfg, NULL,
				      SINGLE_CONFIG);
	else
		ret = sprd_dma_config_linklist(chan, mdesc, mchan->dma_cfg,
					       cfg_count);

	if (ret < 0) {
		spin_lock_irqsave(&mchan->chn_lock, irq_flags);
		list_add_tail(&mdesc->node, &mchan->free);
		spin_unlock_irqrestore(&mchan->chn_lock, irq_flags);
		dev_err(sdev->dma_dev.dev, "DMA configuration is failed\n");
		goto out;
	}

	if (!(flags & DMA_HARDWARE_REQ))
		mchan->dev_id = DMA_SOFTWARE_UID;

	/* add the free descriptor to the prepared list. */
	spin_lock_irqsave(&mchan->chn_lock, irq_flags);
	list_add_tail(&mdesc->node, &mchan->prepared);
	spin_unlock_irqrestore(&mchan->chn_lock, irq_flags);

	mchan->cfg_count = 0;
	memset(mchan->dma_cfg, 0, sizeof(struct sprd_dma_cfg) *
	       SPRD_DMA_CFG_COUNT);
	return &mdesc->desc;

out:
	mchan->cfg_count = 0;
	memset(mchan->dma_cfg, 0, sizeof(struct sprd_dma_cfg) *
	       SPRD_DMA_CFG_COUNT);
	return NULL;
}

static int sprd_dma_pause(struct dma_chan *chan)
{
	struct sprd_dma_chn *mchan =
			container_of(chan, struct sprd_dma_chn, chan);
	unsigned long flags;

	spin_lock_irqsave(&mchan->chn_lock, flags);
	sprd_dma_stop(mchan);
	spin_unlock_irqrestore(&mchan->chn_lock, flags);

	return 0;
}

static int sprd_dma_resume(struct dma_chan *chan)
{
	struct sprd_dma_chn *mchan =
			container_of(chan, struct sprd_dma_chn, chan);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&mchan->chn_lock, flags);
	ret = sprd_dma_start(mchan);
	spin_unlock_irqrestore(&mchan->chn_lock, flags);

	return ret;
}

static int sprd_terminate_all(struct dma_chan *chan)
{
	struct sprd_dma_chn *mchan = to_sprd_dma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&mchan->chn_lock, flags);
	list_splice_tail_init(&mchan->prepared, &mchan->free);
	list_splice_tail_init(&mchan->queued, &mchan->free);
	list_splice_tail_init(&mchan->active, &mchan->free);
	list_splice_tail_init(&mchan->completed, &mchan->free);

	sprd_dma_stop(mchan);
	spin_unlock_irqrestore(&mchan->chn_lock, flags);

	return 0;
}

static int sprd_dma_slave_config(struct dma_chan *chan,
				 struct dma_slave_config *config)
{
	struct sprd_dma_chn *mchan = to_sprd_dma_chan(chan);
	struct sprd_dma_cfg *sprd_cfg = container_of(config,
						     struct sprd_dma_cfg,
						     config);
	int i = 0;

	do {
		memcpy(&mchan->dma_cfg[i], sprd_cfg, sizeof(*sprd_cfg));
		sprd_cfg++;
	} while (mchan->dma_cfg[i++].is_end == DMA_NOT_END
		 && i < (SPRD_DMA_CFG_COUNT - 1));

	mchan->cfg_count = i;
	return 0;
}

static bool sprd_dma_filter_fn(struct dma_chan *chan, void *param)
{
	struct sprd_dma_chn *mchan = to_sprd_dma_chan(chan);
	struct of_phandle_args *dma_spec = (struct of_phandle_args *)param;
	unsigned int req = (unsigned int)dma_spec->args[0];

	if (chan->device->dev->of_node == dma_spec->np)
		return req == (mchan->chn_num + 1);
	else
		return false;
}

static struct dma_chan *sprd_dma_simple_xlate(struct of_phandle_args *dma_spec,
					      struct of_dma *of_dma)
{
	struct of_dma_filter_info *info = of_dma->of_dma_data;
	int count = dma_spec->args_count;

	if (!info || !info->filter_fn)
		return NULL;

	if (count != 1)
		return NULL;

	return dma_request_channel(info->dma_cap, info->filter_fn, dma_spec);
}

static int sprd_dma_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct sprd_dma_dev *sdev;
	struct sprd_dma_chn *dma_chn;
	struct resource *res;
	u32 chn_count;
	int ret, i;

	if (of_property_read_u32(np, "#dma-channels", &chn_count)) {
		dev_err(&pdev->dev, "get dma channels count failed\n");
		return -ENODEV;
	}

	sdev = devm_kzalloc(&pdev->dev, (sizeof(*sdev) +
			    (sizeof(struct sprd_dma_chn) * chn_count)),
			    GFP_KERNEL);
	if (!sdev)
		return -ENOMEM;

	sdev->clk = devm_clk_get(&pdev->dev, "enable");
	if (IS_ERR(sdev->clk)) {
		dev_err(&pdev->dev, "get enable clock failed\n");
		return PTR_ERR(sdev->clk);
	}

	/* ashb clock is optional for AGCP DMA */
	sdev->ashb_clk = devm_clk_get(&pdev->dev, "ashb_eb");
	if (IS_ERR(sdev->ashb_clk))
		dev_warn(&pdev->dev, "get ashb eb clock failed\n");

	sdev->irq = platform_get_irq(pdev, 0);
	if (sdev->irq > 0) {
		ret = devm_request_irq(&pdev->dev, sdev->irq, dma_irq_handle,
				       0, "sprd_dma", (void *)sdev);
		if (ret < 0) {
			dev_err(&pdev->dev, "request dma irq failed\n");
			return ret;
		}

		tasklet_init(&sdev->tasklet, sprd_dma_tasklet,
			     (unsigned long)sdev);
	} else {
		dev_warn(&pdev->dev, "no interrupts for the DMA controller\n");
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	sdev->glb_base = devm_ioremap_nocache(&pdev->dev, res->start,
					      resource_size(res));
	if (!sdev->glb_base)
		return -ENOMEM;

	dma_cap_set(DMA_MEMCPY, sdev->dma_dev.cap_mask);
	sdev->total_chns = chn_count;
	sdev->dma_dev.chancnt = chn_count;
	INIT_LIST_HEAD(&sdev->dma_dev.channels);
	INIT_LIST_HEAD(&sdev->dma_dev.global_node);
	sdev->dma_dev.dev = &pdev->dev;
	sdev->dma_dev.device_alloc_chan_resources = sprd_dma_alloc_chan_resources;
	sdev->dma_dev.device_free_chan_resources = sprd_dma_free_chan_resources;
	sdev->dma_dev.device_tx_status = sprd_dma_tx_status;
	sdev->dma_dev.device_issue_pending = sprd_dma_issue_pending;
	sdev->dma_dev.device_prep_dma_memcpy = sprd_dma_prep_dma_memcpy;
	sdev->dma_dev.device_config = sprd_dma_slave_config;
	sdev->dma_dev.device_pause = sprd_dma_pause;
	sdev->dma_dev.device_resume = sprd_dma_resume;
	sdev->dma_dev.device_terminate_all = sprd_terminate_all;

	for (i = 0; i < chn_count; i++) {
		dma_chn = &sdev->channels[i];
		dma_chn->chan.device = &sdev->dma_dev;
		dma_cookie_init(&dma_chn->chan);
		list_add_tail(&dma_chn->chan.device_node,
			      &sdev->dma_dev.channels);

		dma_chn->chn_num = i;
		/* get each channel's registers base address. */
		dma_chn->chn_base = (void __iomem *)
			((unsigned long)sdev->glb_base +
			 SPRD_DMA_CHN_REG_OFFSET +
			 SPRD_DMA_CHN_REG_LENGTH * i);

		spin_lock_init(&dma_chn->chn_lock);
		INIT_LIST_HEAD(&dma_chn->free);
		INIT_LIST_HEAD(&dma_chn->prepared);
		INIT_LIST_HEAD(&dma_chn->queued);
		INIT_LIST_HEAD(&dma_chn->active);
		INIT_LIST_HEAD(&dma_chn->completed);
	}

	sdev->dma_desc_node_cachep = kmem_cache_create("dma_desc_node",
						sizeof(struct sprd_dma_desc), 0,
						SLAB_HWCACHE_ALIGN, NULL);
	if (!sdev->dma_desc_node_cachep) {
		dev_err(&pdev->dev, "allocate memory cache failed\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, sdev);
	ret = sprd_dma_enable(sdev);
	if (ret)
		goto err_enable;

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	ret = pm_runtime_get_sync(&pdev->dev);
	if (ret < 0)
		goto err_rpm;

	ret = dma_async_device_register(&sdev->dma_dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "register dma device failed:%d\n", ret);
		goto err_register;
	}

	sprd_dma_info.dma_cap = sdev->dma_dev.cap_mask;
	ret = of_dma_controller_register(np, sprd_dma_simple_xlate,
					 &sprd_dma_info);
	if (ret) {
		dev_err(&pdev->dev, "failed to register of DMA controller\n");
		goto err_of_register;
	}

	pm_runtime_put_sync(&pdev->dev);
	return 0;

err_of_register:
	dma_async_device_unregister(&sdev->dma_dev);
err_register:
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
err_rpm:
	sprd_dma_disable(sdev);
err_enable:
	kmem_cache_destroy(sdev->dma_desc_node_cachep);
	return ret;
}

static int sprd_dma_remove(struct platform_device *pdev)
{
	struct sprd_dma_dev *sdev = platform_get_drvdata(pdev);
	int ret;

	ret = pm_runtime_get_sync(&pdev->dev);
	if (ret < 0)
		return ret;

	kmem_cache_destroy(sdev->dma_desc_node_cachep);
	dma_async_device_unregister(&sdev->dma_dev);
	sprd_dma_disable(sdev);

	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	return 0;
}

static const struct of_device_id sprd_dma_match[] = {
	{ .compatible = "sprd,sc9860-dma", },
	{},
};

static int __maybe_unused sprd_dma_runtime_suspend(struct device *dev)
{
	struct sprd_dma_dev *sdev = dev_get_drvdata(dev);

	sprd_dma_disable(sdev);
	return 0;
}

static int __maybe_unused sprd_dma_runtime_resume(struct device *dev)
{
	struct sprd_dma_dev *sdev = dev_get_drvdata(dev);

	return sprd_dma_enable(sdev);
}

static const struct dev_pm_ops sprd_dma_pm_ops = {
	SET_RUNTIME_PM_OPS(sprd_dma_runtime_suspend,
			   sprd_dma_runtime_resume,
			   NULL)
};

static struct platform_driver sprd_dma_driver = {
	.probe = sprd_dma_probe,
	.remove = sprd_dma_remove,
	.driver = {
		.name = "sprd-dma",
		.owner = THIS_MODULE,
		.of_match_table = sprd_dma_match,
		.pm = &sprd_dma_pm_ops,
	},
};

static int __init sprd_dma_init(void)
{
	return platform_driver_register(&sprd_dma_driver);
}
arch_initcall_sync(sprd_dma_init);

static void __exit sprd_dma_exit(void)
{
	platform_driver_unregister(&sprd_dma_driver);
}
module_exit(sprd_dma_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DMA driver for Spreadtrum");
MODULE_AUTHOR("Baolin Wang <baolin.wang@spreadtrum.com>");
MODULE_AUTHOR("Eric Long <eric.long@spreadtrum.com>");
