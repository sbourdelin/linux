// SPDX-License-Identifier: GPL-2.0
// Copyright 2018 NXP

/*
 * Driver for NXP Layerscape Queue Direct Memory Access Controller
 *
 * Author:
 *  Wen He <wen.he_1@nxp.com>
 *  Jiaheng Fan <jiaheng.fan@nxp.com>
 *
 */

#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/of_dma.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/dmaengine.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "virt-dma.h"
#include "fsldma.h"

/* Register related definition */
#define FSL_QDMA_DMR			0x0
#define FSL_QDMA_DSR			0x4
#define FSL_QDMA_DEIER			0xe00
#define FSL_QDMA_DEDR			0xe04
#define FSL_QDMA_DECFDW0R		0xe10
#define FSL_QDMA_DECFDW1R		0xe14
#define FSL_QDMA_DECFDW2R		0xe18
#define FSL_QDMA_DECFDW3R		0xe1c
#define FSL_QDMA_DECFQIDR		0xe30
#define FSL_QDMA_DECBR			0xe34

#define FSL_QDMA_BCQMR(x)		(0xc0 + 0x100 * (x))
#define FSL_QDMA_BCQSR(x)		(0xc4 + 0x100 * (x))
#define FSL_QDMA_BCQEDPA_SADDR(x)	(0xc8 + 0x100 * (x))
#define FSL_QDMA_BCQDPA_SADDR(x)	(0xcc + 0x100 * (x))
#define FSL_QDMA_BCQEEPA_SADDR(x)	(0xd0 + 0x100 * (x))
#define FSL_QDMA_BCQEPA_SADDR(x)	(0xd4 + 0x100 * (x))
#define FSL_QDMA_BCQIER(x)		(0xe0 + 0x100 * (x))
#define FSL_QDMA_BCQIDR(x)		(0xe4 + 0x100 * (x))

#define FSL_QDMA_SQDPAR			0x80c
#define FSL_QDMA_SQEPAR			0x814
#define FSL_QDMA_BSQMR			0x800
#define FSL_QDMA_BSQSR			0x804
#define FSL_QDMA_BSQICR			0x828
#define FSL_QDMA_CQMR			0xa00
#define FSL_QDMA_CQDSCR1		0xa08
#define FSL_QDMA_CQDSCR2                0xa0c
#define FSL_QDMA_CQIER			0xa10
#define FSL_QDMA_CQEDR			0xa14
#define FSL_QDMA_SQCCMR			0xa20

/* Registers for bit and genmask */
#define FSL_QDMA_CQIDR_SQT		BIT(15)
#define QDMA_CCDF_FOTMAT		BIT(29)
#define QDMA_CCDF_SER			BIT(30)
#define QDMA_SG_FIN			BIT(30)
#define QDMA_SG_EXT			BIT(31)
#define QDMA_SG_LEN_MASK		GENMASK(29, 0)
#define QDMA_CCDF_MASK			GENMASK(28, 20)

#define FSL_QDMA_DEDR_CLEAR		GENMASK(31, 0)
#define FSL_QDMA_BCQIDR_CLEAR		GENMASK(31, 0)
#define FSL_QDMA_DEIER_CLEAR		GENMASK(31, 0)

#define FSL_QDMA_BCQIER_CQTIE		BIT(15)
#define FSL_QDMA_BCQIER_CQPEIE		BIT(23)
#define FSL_QDMA_BSQICR_ICEN		BIT(31)

#define FSL_QDMA_BSQICR_ICST(x)		((x) << 16)
#define FSL_QDMA_CQIER_MEIE		BIT(31)
#define FSL_QDMA_CQIER_TEIE		BIT(0)
#define FSL_QDMA_SQCCMR_ENTER_WM	BIT(21)

#define FSL_QDMA_BCQMR_EN		BIT(31)
#define FSL_QDMA_BCQMR_EI		BIT(30)
#define FSL_QDMA_BCQMR_CD_THLD(x)	((x) << 20)
#define FSL_QDMA_BCQMR_CQ_SIZE(x)	((x) << 16)

#define FSL_QDMA_BCQSR_QF		BIT(16)
#define FSL_QDMA_BCQSR_XOFF		BIT(0)

#define FSL_QDMA_BSQMR_EN		BIT(31)
#define FSL_QDMA_BSQMR_DI		BIT(30)
#define FSL_QDMA_BSQMR_CQ_SIZE(x)	((x) << 16)

#define FSL_QDMA_BSQSR_QE		BIT(17)

#define FSL_QDMA_DMR_DQD		BIT(30)
#define FSL_QDMA_DSR_DB		BIT(31)

/* Size related definition */
#define FSL_QDMA_QUEUE_MAX		8
#define FSL_QDMA_BASE_BUFFER_SIZE	96
#define FSL_QDMA_CIRCULAR_DESC_SIZE_MIN	64
#define FSL_QDMA_CIRCULAR_DESC_SIZE_MAX	16384
#define FSL_QDMA_QUEUE_NUM_MAX		8

/* Field definition for CMD */
#define FSL_QDMA_CMD_RWTTYPE		0x4
#define FSL_QDMA_CMD_LWC                0x2
#define FSL_QDMA_CMD_RWTTYPE_OFFSET	28
#define FSL_QDMA_CMD_NS_OFFSET		27
#define FSL_QDMA_CMD_DQOS_OFFSET	24
#define FSL_QDMA_CMD_WTHROTL_OFFSET	20
#define FSL_QDMA_CMD_DSEN_OFFSET	19
#define FSL_QDMA_CMD_LWC_OFFSET		16

#define FSL_QDMA_E_SG_TABLE		1
#define FSL_QDMA_E_DATA_BUFFER		0
#define FSL_QDMA_F_LAST_ENTRY		1

/* Field definition for Descriptor offset */
#define QDMA_CCDF_STATUS		20
#define QDMA_CCDF_OFFSET		20

/**
 * struct fsl_qdma_format - This is the struct holding describing compound
 *			    descriptor format with qDMA.
 * @status:		    Command status and enqueue status notification.
 * @cfg:		    Frame offset and frame format.
 * @addr_lo:		    Holding the compound descriptor of the lower
 *			    32-bits address in memory 40-bit address.
 * @addr_hi:		    Same as above member, but point high 8-bits in
 *			    memory 40-bit address.
 * @__reserved1:	    Reserved field.
 * @cfg8b_w1:		    Compound descriptor command queue origin produced
 *			    by qDMA and dynamic debug field.
 * @data		    Pointer to the memory 40-bit address, describes DMA
 *			    source information and DMA destination information.
 */
struct fsl_qdma_format {
	__le32 status;
	__le32 cfg;
	union {
		struct {
			__le32 addr_lo;
			u8 addr_hi;
			u8 __reserved1[2];
			u8 cfg8b_w1;
		} __packed;
		__le64 data;
	};
} __packed;

/* qDMA status notification pre information */
struct fsl_pre_status {
	u64 queue;
	u64 addr;
};

struct fsl_qdma_chan {
	struct virt_dma_chan		vchan;
	struct virt_dma_desc		vdesc;
	enum dma_status			status;
	u32				slave_id;
	struct fsl_qdma_engine		*qdma;
	struct fsl_qdma_queue		*queue;
	struct list_head		qcomp;
};

struct fsl_qdma_queue {
	struct fsl_qdma_format	*virt_head;
	struct fsl_qdma_format	*virt_tail;
	struct list_head	comp_used;
	struct list_head	comp_free;
	struct dma_pool		*comp_pool;
	spinlock_t		queue_lock;
	dma_addr_t		bus_addr;
	u32                     n_cq;
	u32			id;
	struct fsl_qdma_format	*cq;
};

struct fsl_qdma_comp {
	dma_addr_t              bus_addr;
	struct fsl_qdma_format	*virt_addr;
	struct fsl_qdma_chan	*qchan;
	struct virt_dma_desc    vdesc;
	struct list_head	list;
};

struct fsl_qdma_engine {
	struct dma_device	dma_dev;
	void __iomem		*ctrl_base;
	void __iomem            *status_base;
	void __iomem		*block_base;
	u32			n_chans;
	u32			n_queues;
	struct mutex            fsl_qdma_mutex;
	int			error_irq;
	int			queue_irq;
	bool			feature;
	struct fsl_qdma_queue	*queue;
	struct fsl_qdma_queue	*status;
	struct fsl_qdma_chan	chans[];

};

static inline u64
qdma_ccdf_addr_get64(const struct fsl_qdma_format *ccdf)
{
	return le64_to_cpu(ccdf->data) & (U64_MAX >> 24);
}

static inline void
qdma_desc_addr_set64(struct fsl_qdma_format *ccdf, u64 addr)
{
	ccdf->addr_hi = upper_32_bits(addr);
	ccdf->addr_lo = cpu_to_le32(lower_32_bits(addr));
}

static inline u64
qdma_ccdf_get_queue(const struct fsl_qdma_format *ccdf)
{
	return ccdf->cfg8b_w1 & U8_MAX;
}

static inline int
qdma_ccdf_get_offset(const struct fsl_qdma_format *ccdf)
{
	return (le32_to_cpu(ccdf->cfg) & QDMA_CCDF_MASK) >> QDMA_CCDF_OFFSET;
}

static inline void
qdma_ccdf_set_format(struct fsl_qdma_format *ccdf, int offset)
{
	ccdf->cfg = cpu_to_le32(QDMA_CCDF_FOTMAT | offset);
}

static inline int
qdma_ccdf_get_status(const struct fsl_qdma_format *ccdf)
{
	return (le32_to_cpu(ccdf->status) & QDMA_CCDF_MASK) >> QDMA_CCDF_STATUS;
}

static inline void
qdma_ccdf_set_ser(struct fsl_qdma_format *ccdf, int status)
{
	ccdf->status = cpu_to_le32(QDMA_CCDF_SER | status);
}

static inline void qdma_csgf_set_len(struct fsl_qdma_format *csgf, int len)
{
	csgf->cfg = cpu_to_le32(len & QDMA_SG_LEN_MASK);
}

static inline void qdma_csgf_set_f(struct fsl_qdma_format *csgf, int len)
{
	csgf->cfg = cpu_to_le32(QDMA_SG_FIN | (len & QDMA_SG_LEN_MASK));
}

static inline void qdma_csgf_set_e(struct fsl_qdma_format *csgf, int len)
{
	csgf->cfg = cpu_to_le32(QDMA_SG_EXT | (len & QDMA_SG_LEN_MASK));
}

static u32 qdma_readl(struct fsl_qdma_engine *qdma, void __iomem *addr)
{
	return FSL_DMA_IN(qdma, addr, 32);
}

static void qdma_writel(struct fsl_qdma_engine *qdma, u32 val,
						void __iomem *addr)
{
	FSL_DMA_OUT(qdma, addr, val, 32);
}

static struct fsl_qdma_chan *to_fsl_qdma_chan(struct dma_chan *chan)
{
	return container_of(chan, struct fsl_qdma_chan, vchan.chan);
}

static struct fsl_qdma_comp *to_fsl_qdma_comp(struct virt_dma_desc *vd)
{
	return container_of(vd, struct fsl_qdma_comp, vdesc);
}

static void fsl_qdma_free_chan_resources(struct dma_chan *chan)
{
	struct fsl_qdma_chan *fsl_chan = to_fsl_qdma_chan(chan);
	unsigned long flags;
	LIST_HEAD(head);

	spin_lock_irqsave(&fsl_chan->vchan.lock, flags);
	vchan_get_all_descriptors(&fsl_chan->vchan, &head);
	spin_unlock_irqrestore(&fsl_chan->vchan.lock, flags);

	vchan_dma_desc_free_list(&fsl_chan->vchan, &head);
}

static void fsl_qdma_comp_fill_memcpy(struct fsl_qdma_comp *fsl_comp,
					dma_addr_t dst, dma_addr_t src, u32 len)
{
	struct fsl_qdma_format *ccdf, *csgf_desc, *csgf_src, *csgf_dest;
	struct fsl_qdma_format *sdf, *ddf;

	ccdf = fsl_comp->virt_addr;
	csgf_desc = fsl_comp->virt_addr + 1;
	csgf_src = fsl_comp->virt_addr + 2;
	csgf_dest = fsl_comp->virt_addr + 3;
	sdf = fsl_comp->virt_addr + 4;
	ddf = fsl_comp->virt_addr + 5;

	memset(fsl_comp->virt_addr, 0, FSL_QDMA_BASE_BUFFER_SIZE);
	/* Head Command Descriptor(Frame Descriptor) */
	qdma_desc_addr_set64(ccdf, fsl_comp->bus_addr + 16);
	qdma_ccdf_set_format(ccdf, qdma_ccdf_get_offset(ccdf));
	qdma_ccdf_set_ser(ccdf, qdma_ccdf_get_status(ccdf));

	/* Status notification is enqueued to status queue. */
	/* Compound Command Descriptor(Frame List Table) */
	qdma_desc_addr_set64(csgf_desc, fsl_comp->bus_addr + 64);
	/* It must be 32 as Compound S/G Descriptor */
	qdma_csgf_set_len(csgf_desc, 32);
	qdma_desc_addr_set64(csgf_src, src);
	qdma_csgf_set_len(csgf_src, len);
	qdma_desc_addr_set64(csgf_dest, dst);
	qdma_csgf_set_len(csgf_dest, len);
	/* This entry is the last entry. */
	qdma_csgf_set_f(csgf_dest, len);
	/* Descriptor Buffer */
	sdf->data = cpu_to_le64(
			FSL_QDMA_CMD_RWTTYPE << FSL_QDMA_CMD_RWTTYPE_OFFSET);
	ddf->data = cpu_to_le64(
			FSL_QDMA_CMD_RWTTYPE << FSL_QDMA_CMD_RWTTYPE_OFFSET);
	ddf->data |= cpu_to_le64(
			FSL_QDMA_CMD_LWC << FSL_QDMA_CMD_LWC_OFFSET);
}

/*
 * Pre-request full command descriptor for enqueue.
 */
static int fsl_qdma_pre_request_enqueue_desc(struct fsl_qdma_queue *queue)
{
	struct fsl_qdma_comp *comp_temp, *_comp_temp;
	int i;

	for (i = 0; i < queue->n_cq; i++) {
		comp_temp = kzalloc(sizeof(*comp_temp), GFP_KERNEL);
		if (!comp_temp)
			goto err;

		comp_temp->virt_addr = dma_pool_alloc(queue->comp_pool,
						      GFP_KERNEL,
						      &comp_temp->bus_addr);
		if (!comp_temp->virt_addr)
			goto err;

		list_add_tail(&comp_temp->list, &queue->comp_free);
	}
	return 0;

err:
	if (i == 0 && comp_temp) {
		kfree(comp_temp);
		return -ENOMEM;
	}

	while (--i >= 1) {
		list_for_each_entry_safe(comp_temp, _comp_temp,
				&queue->comp_free, list) {
			dma_pool_free(queue->comp_pool,
					comp_temp->virt_addr,
					comp_temp->bus_addr);
			list_del(&comp_temp->list);
			kfree(comp_temp);
		}
	}
	return -ENOMEM;
}

/*
 * Request a command descriptor for enqueue.
 */
static struct fsl_qdma_comp *fsl_qdma_request_enqueue_desc(
					struct fsl_qdma_chan *fsl_chan,
					unsigned int dst_nents,
					unsigned int src_nents)
{
	struct fsl_qdma_comp *comp_temp;
	struct fsl_qdma_queue *queue = fsl_chan->queue;
	unsigned long flags;

	spin_lock_irqsave(&queue->queue_lock, flags);
	if (list_empty(&queue->comp_free)) {
		spin_unlock_irqrestore(&queue->queue_lock, flags);
		comp_temp = kzalloc(sizeof(*comp_temp), GFP_KERNEL);
		if (!comp_temp)
			return NULL;

		comp_temp->virt_addr = dma_pool_alloc(queue->comp_pool,
						      GFP_KERNEL,
						      &comp_temp->bus_addr);
		if (!comp_temp->virt_addr) {
			kfree(comp_temp);
			return NULL;
		}

	} else {
		comp_temp = list_first_entry(&queue->comp_free,
					     struct fsl_qdma_comp,
					     list);
		list_del(&comp_temp->list);
		spin_unlock_irqrestore(&queue->queue_lock, flags);
	}

	comp_temp->qchan = fsl_chan;

	return comp_temp;
}

static struct fsl_qdma_queue *fsl_qdma_alloc_queue_resources(
					struct platform_device *pdev,
					unsigned int queue_num)
{
	struct fsl_qdma_queue *queue_head, *queue_temp;
	int ret, len, i;
	unsigned int queue_size[FSL_QDMA_QUEUE_MAX];

	if (queue_num > FSL_QDMA_QUEUE_MAX)
		queue_num = FSL_QDMA_QUEUE_MAX;
	len = sizeof(*queue_head) * queue_num;
	queue_head = devm_kzalloc(&pdev->dev, len, GFP_KERNEL);
	if (!queue_head)
		return NULL;

	ret = device_property_read_u32_array(&pdev->dev, "queue-sizes",
					queue_size, queue_num);
	if (ret) {
		dev_err(&pdev->dev, "Can't get queue-sizes.\n");
		return NULL;
	}

	for (i = 0; i < queue_num; i++) {
		if (queue_size[i] > FSL_QDMA_CIRCULAR_DESC_SIZE_MAX ||
			    queue_size[i] < FSL_QDMA_CIRCULAR_DESC_SIZE_MIN) {
			dev_err(&pdev->dev, "Get wrong queue-sizes.\n");
			return NULL;
		}
		queue_temp = queue_head + i;
		queue_temp->cq = dma_alloc_coherent(&pdev->dev,
						sizeof(struct fsl_qdma_format) *
						queue_size[i],
						&queue_temp->bus_addr,
						GFP_KERNEL);
		if (!queue_temp->cq) {
			devm_kfree(&pdev->dev, queue_head);
			return NULL;
		}
		queue_temp->n_cq = queue_size[i];
		queue_temp->id = i;
		queue_temp->virt_head = queue_temp->cq;
		queue_temp->virt_tail = queue_temp->cq;

		/*
		 * Create a comp dma pool that size
		 * is 'FSL_QDMA_BASE_BUFFER_SIZE'.
		 * The dma pool for queue command buffer.
		 */
		queue_temp->comp_pool = dma_pool_create("comp_pool",
						&pdev->dev,
						FSL_QDMA_BASE_BUFFER_SIZE,
						16, 0);
		if (!queue_temp->comp_pool)
			goto err;

		/*
		 * List for queue command buffer
		 */
		INIT_LIST_HEAD(&queue_temp->comp_used);
		INIT_LIST_HEAD(&queue_temp->comp_free);
		spin_lock_init(&queue_temp->queue_lock);
	}

	return queue_head;

err:
	if (i == 0 && queue_temp->comp_pool)
		dma_pool_destroy(queue_temp->comp_pool);
	while (--i >= 1) {
		queue_temp = queue_head + i;
		if (i == 1 && unlikely(queue_temp->comp_pool))
			dma_pool_destroy(queue_temp->comp_pool);
	}

	dev_err(&pdev->dev,
		"unable to allocate channel %d descriptor pool\n",
		queue_temp->id);

	while (--i >= 0) {
		queue_temp = queue_head + i;
		dma_free_coherent(&pdev->dev,
				sizeof(struct fsl_qdma_format) *
				queue_size[i],
				queue_temp->cq,
				queue_temp->bus_addr);
	}
	devm_kfree(&pdev->dev, queue_head);
	return NULL;
}

static struct fsl_qdma_queue *fsl_qdma_prep_status_queue(
						struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct fsl_qdma_queue *status_head;
	unsigned int status_size;
	int ret;

	ret = of_property_read_u32(np, "status-sizes", &status_size);
	if (ret) {
		dev_err(&pdev->dev, "Can't get status-sizes.\n");
		return NULL;
	}
	if (status_size > FSL_QDMA_CIRCULAR_DESC_SIZE_MAX
			|| status_size < FSL_QDMA_CIRCULAR_DESC_SIZE_MIN) {
		dev_err(&pdev->dev, "Get wrong status_size.\n");
		return NULL;
	}
	status_head = devm_kzalloc(&pdev->dev, sizeof(*status_head),
								GFP_KERNEL);
	if (!status_head)
		return NULL;

	/*
	 * Buffer for queue command
	 */
	status_head->cq = dma_alloc_coherent(&pdev->dev,
						sizeof(struct fsl_qdma_format) *
						status_size,
						&status_head->bus_addr,
						GFP_KERNEL);
	if (!status_head->cq) {
		devm_kfree(&pdev->dev, status_head);
		return NULL;
	}

	status_head->n_cq = status_size;
	status_head->virt_head = status_head->cq;
	status_head->virt_tail = status_head->cq;
	status_head->comp_pool = NULL;

	return status_head;
}

static int fsl_qdma_halt(struct fsl_qdma_engine *fsl_qdma)
{
	void __iomem *ctrl = fsl_qdma->ctrl_base;
	void __iomem *block = fsl_qdma->block_base;
	int i, count = 5;
	u32 reg;

	/* Disable the command queue and wait for idle state. */
	reg = qdma_readl(fsl_qdma, ctrl + FSL_QDMA_DMR);
	reg |= FSL_QDMA_DMR_DQD;
	qdma_writel(fsl_qdma, reg, ctrl + FSL_QDMA_DMR);
	for (i = 0; i < FSL_QDMA_QUEUE_NUM_MAX; i++)
		qdma_writel(fsl_qdma, 0, block + FSL_QDMA_BCQMR(i));

	while (1) {
		reg = qdma_readl(fsl_qdma, ctrl + FSL_QDMA_DSR);
		if (!(reg & FSL_QDMA_DSR_DB))
			break;
		if (count-- < 0)
			return -EBUSY;
		udelay(100);
	}

	/* Disable status queue. */
	qdma_writel(fsl_qdma, 0, block + FSL_QDMA_BSQMR);

	/* Clear all detected events and interrupts for all queues. */
	qdma_writel(fsl_qdma, FSL_QDMA_BCQIDR_CLEAR,
				block + FSL_QDMA_BCQIDR(0));

	return 0;
}

static int fsl_qdma_queue_transfer_complete(struct fsl_qdma_engine *fsl_qdma)
{
	struct fsl_qdma_queue *fsl_queue = fsl_qdma->queue;
	struct fsl_qdma_queue *fsl_status = fsl_qdma->status;
	struct fsl_qdma_queue *temp_queue;
	struct fsl_qdma_comp *fsl_comp;
	struct fsl_qdma_format *status_addr;
	struct fsl_qdma_format *csgf_src;
	struct fsl_pre_status pre;
	void __iomem *block = fsl_qdma->block_base;
	u32 reg, i;
	bool duplicate, duplicate_handle;

	memset(&pre, 0, sizeof(struct fsl_pre_status));

	while (1) {
		duplicate = 0;
		duplicate_handle = 0;
		reg = qdma_readl(fsl_qdma, block + FSL_QDMA_BSQSR);
		if (reg & FSL_QDMA_BSQSR_QE)
			return 0;
		status_addr = fsl_status->virt_head;
		if (qdma_ccdf_get_queue(status_addr) == pre.queue &&
			qdma_ccdf_addr_get64(status_addr) == pre.addr)
			duplicate = 1;
		i = qdma_ccdf_get_queue(status_addr);
		pre.queue = qdma_ccdf_get_queue(status_addr);
		pre.addr = qdma_ccdf_addr_get64(status_addr);
		temp_queue = fsl_queue + i;
		spin_lock(&temp_queue->queue_lock);
		if (list_empty(&temp_queue->comp_used)) {
			if (duplicate) {
				duplicate_handle = 1;
			} else {
				spin_unlock(&temp_queue->queue_lock);
				return -EAGAIN;
			}
		} else {
			fsl_comp = list_first_entry(&temp_queue->comp_used,
							struct fsl_qdma_comp,
							list);
			csgf_src = fsl_comp->virt_addr + 2;
			if (fsl_comp->bus_addr + 16 != pre.addr) {
				if (duplicate) {
					duplicate_handle = 1;
				} else {
					spin_unlock(&temp_queue->queue_lock);
					return -EAGAIN;
				}
			}
		}

			if (duplicate_handle) {
				reg = qdma_readl(fsl_qdma, block +
						FSL_QDMA_BSQMR);
			reg |= FSL_QDMA_BSQMR_DI;
			qdma_desc_addr_set64(status_addr, 0x0);
			fsl_status->virt_head++;
			if (fsl_status->virt_head == fsl_status->cq
						   + fsl_status->n_cq)
				fsl_status->virt_head = fsl_status->cq;
			qdma_writel(fsl_qdma, reg, block + FSL_QDMA_BSQMR);
			spin_unlock(&temp_queue->queue_lock);
			continue;
		}
		list_del(&fsl_comp->list);

		reg = qdma_readl(fsl_qdma, block + FSL_QDMA_BSQMR);
		reg |= FSL_QDMA_BSQMR_DI;
		qdma_desc_addr_set64(status_addr, 0x0);
		fsl_status->virt_head++;
		if (fsl_status->virt_head == fsl_status->cq + fsl_status->n_cq)
			fsl_status->virt_head = fsl_status->cq;
		qdma_writel(fsl_qdma, reg, block + FSL_QDMA_BSQMR);
		spin_unlock(&temp_queue->queue_lock);

		spin_lock(&fsl_comp->qchan->vchan.lock);
		vchan_cookie_complete(&fsl_comp->vdesc);
		fsl_comp->qchan->status = DMA_COMPLETE;
		spin_unlock(&fsl_comp->qchan->vchan.lock);
	}

	return 0;
}

static irqreturn_t fsl_qdma_error_handler(int irq, void *dev_id)
{
	struct fsl_qdma_engine *fsl_qdma = dev_id;
	unsigned int intr;
	void __iomem *status = fsl_qdma->status_base;

	intr = qdma_readl(fsl_qdma, status + FSL_QDMA_DEDR);

	if (intr)
		dev_err(fsl_qdma->dma_dev.dev, "DMA transaction error!\n");

	/* Clear all error conditions and interrupts. */
	qdma_writel(fsl_qdma, FSL_QDMA_DEDR_CLEAR, status + FSL_QDMA_DEDR);

	return IRQ_HANDLED;
}

static irqreturn_t fsl_qdma_queue_handler(int irq, void *dev_id)
{
	struct fsl_qdma_engine *fsl_qdma = dev_id;
	unsigned int intr, reg;
	void __iomem *block = fsl_qdma->block_base;
	void __iomem *ctrl = fsl_qdma->ctrl_base;

	intr = qdma_readl(fsl_qdma, block + FSL_QDMA_BCQIDR(0));

	if ((intr & FSL_QDMA_CQIDR_SQT) != 0)
		intr = fsl_qdma_queue_transfer_complete(fsl_qdma);

	if (intr != 0) {
		reg = qdma_readl(fsl_qdma, ctrl + FSL_QDMA_DMR);
		reg |= FSL_QDMA_DMR_DQD;
		qdma_writel(fsl_qdma, reg, ctrl + FSL_QDMA_DMR);
		qdma_writel(fsl_qdma, 0, block + FSL_QDMA_BCQIER(0));
		dev_err(fsl_qdma->dma_dev.dev, "QDMA: status err!\n");
	}

	/* Clear all detected events and interrupts. */
	qdma_writel(fsl_qdma, FSL_QDMA_BCQIDR_CLEAR,
				block + FSL_QDMA_BCQIDR(0));

	return IRQ_HANDLED;
}

static int
fsl_qdma_irq_init(struct platform_device *pdev,
		  struct fsl_qdma_engine *fsl_qdma)
{
	int ret;

	fsl_qdma->error_irq = platform_get_irq_byname(pdev,
							"qdma-error");
	if (fsl_qdma->error_irq < 0) {
		dev_err(&pdev->dev, "Can't get qdma controller irq.\n");
		return fsl_qdma->error_irq;
	}

	fsl_qdma->queue_irq = platform_get_irq_byname(pdev, "qdma-queue");
	if (fsl_qdma->queue_irq < 0) {
		dev_err(&pdev->dev, "Can't get qdma queue irq.\n");
		return fsl_qdma->queue_irq;
	}

	ret = devm_request_irq(&pdev->dev, fsl_qdma->error_irq,
			fsl_qdma_error_handler, 0, "qDMA error", fsl_qdma);
	if (ret) {
		dev_err(&pdev->dev, "Can't register qDMA controller IRQ.\n");
		return  ret;
	}
	ret = devm_request_irq(&pdev->dev, fsl_qdma->queue_irq,
			fsl_qdma_queue_handler, 0, "qDMA queue", fsl_qdma);
	if (ret) {
		dev_err(&pdev->dev, "Can't register qDMA queue IRQ.\n");
		return  ret;
	}

	return 0;
}

static void fsl_qdma_irq_exit(
		struct platform_device *pdev, struct fsl_qdma_engine *fsl_qdma)
{
	if (fsl_qdma->queue_irq == fsl_qdma->error_irq) {
		devm_free_irq(&pdev->dev, fsl_qdma->queue_irq, fsl_qdma);
	} else {
		devm_free_irq(&pdev->dev, fsl_qdma->queue_irq, fsl_qdma);
		devm_free_irq(&pdev->dev, fsl_qdma->error_irq, fsl_qdma);
	}
}

static int fsl_qdma_reg_init(struct fsl_qdma_engine *fsl_qdma)
{
	struct fsl_qdma_queue *fsl_queue = fsl_qdma->queue;
	struct fsl_qdma_queue *temp;
	void __iomem *ctrl = fsl_qdma->ctrl_base;
	void __iomem *status = fsl_qdma->status_base;
	void __iomem *block = fsl_qdma->block_base;
	int i, ret;
	u32 reg;

	/* Try to halt the qDMA engine first. */
	ret = fsl_qdma_halt(fsl_qdma);
	if (ret) {
		dev_err(fsl_qdma->dma_dev.dev, "DMA halt failed!");
		return ret;
	}

	/* Clear all detected events and interrupts for all queues. */
	qdma_writel(fsl_qdma, FSL_QDMA_BCQIDR_CLEAR,
				block + FSL_QDMA_BCQIDR(0));

	for (i = 0; i < fsl_qdma->n_queues; i++) {
		temp = fsl_queue + i;
		/*
		 * Initialize Command Queue registers to point to the first
		 * command descriptor in memory.
		 * Dequeue Pointer Address Registers
		 * Enqueue Pointer Address Registers
		 */
		qdma_writel(fsl_qdma, temp->bus_addr,
				block + FSL_QDMA_BCQDPA_SADDR(i));
		qdma_writel(fsl_qdma, temp->bus_addr,
				block + FSL_QDMA_BCQEPA_SADDR(i));

		/* Initialize the queue mode. */
		reg = FSL_QDMA_BCQMR_EN;
		reg |= FSL_QDMA_BCQMR_CD_THLD(ilog2(temp->n_cq) - 4);
		reg |= FSL_QDMA_BCQMR_CQ_SIZE(ilog2(temp->n_cq) - 6);
		qdma_writel(fsl_qdma, reg, block + FSL_QDMA_BCQMR(i));
	}

	/*
	 * Workaround for erratum: ERR010812.
	 * We must enable XOFF to avoid the enqueue rejection occurs.
	 * Setting SQCCMR ENTER_WM to 0x20.
	 */
	qdma_writel(fsl_qdma, FSL_QDMA_SQCCMR_ENTER_WM,
			      block + FSL_QDMA_SQCCMR);
	/*
	 * Initialize status queue registers to point to the first
	 * command descriptor in memory.
	 * Dequeue Pointer Address Registers
	 * Enqueue Pointer Address Registers
	 */
	qdma_writel(fsl_qdma, fsl_qdma->status->bus_addr,
					block + FSL_QDMA_SQEPAR);
	qdma_writel(fsl_qdma, fsl_qdma->status->bus_addr,
					block + FSL_QDMA_SQDPAR);
	/* Initialize status queue interrupt. */
	qdma_writel(fsl_qdma, FSL_QDMA_BCQIER_CQTIE,
			      block + FSL_QDMA_BCQIER(0));
	qdma_writel(fsl_qdma, FSL_QDMA_BSQICR_ICEN | FSL_QDMA_BSQICR_ICST(5)
						   | 0x8000,
			      block + FSL_QDMA_BSQICR);
	qdma_writel(fsl_qdma, FSL_QDMA_CQIER_MEIE | FSL_QDMA_CQIER_TEIE,
			      block + FSL_QDMA_CQIER);
	/* Initialize controller interrupt register. */
	qdma_writel(fsl_qdma, FSL_QDMA_DEDR_CLEAR, status + FSL_QDMA_DEDR);
	qdma_writel(fsl_qdma, FSL_QDMA_DEIER_CLEAR, status + FSL_QDMA_DEIER);

	/* Initialize the status queue mode. */
	reg = FSL_QDMA_BSQMR_EN;
	reg |= FSL_QDMA_BSQMR_CQ_SIZE(ilog2(fsl_qdma->status->n_cq)-6);
	qdma_writel(fsl_qdma, reg, block + FSL_QDMA_BSQMR);

	reg = qdma_readl(fsl_qdma, ctrl + FSL_QDMA_DMR);
	reg &= ~FSL_QDMA_DMR_DQD;
	qdma_writel(fsl_qdma, reg, ctrl + FSL_QDMA_DMR);

	return 0;
}

static struct dma_async_tx_descriptor *
fsl_qdma_prep_memcpy(struct dma_chan *chan, dma_addr_t dst,
		dma_addr_t src, size_t len, unsigned long flags)
{
	struct fsl_qdma_chan *fsl_chan = to_fsl_qdma_chan(chan);
	struct fsl_qdma_comp *fsl_comp;

	fsl_comp = fsl_qdma_request_enqueue_desc(fsl_chan, 0, 0);
	fsl_qdma_comp_fill_memcpy(fsl_comp, dst, src, len);

	return vchan_tx_prep(&fsl_chan->vchan, &fsl_comp->vdesc, flags);
}

static void fsl_qdma_enqueue_desc(struct fsl_qdma_chan *fsl_chan)
{
	void __iomem *block = fsl_chan->qdma->block_base;
	struct fsl_qdma_queue *fsl_queue = fsl_chan->queue;
	struct fsl_qdma_comp *fsl_comp;
	struct virt_dma_desc *vdesc;
	u32 reg;

	reg = qdma_readl(fsl_chan->qdma, block + FSL_QDMA_BCQSR(fsl_queue->id));
	if (reg & (FSL_QDMA_BCQSR_QF | FSL_QDMA_BCQSR_XOFF))
		return;
	vdesc = vchan_next_desc(&fsl_chan->vchan);
	if (!vdesc)
		return;
	list_del(&vdesc->node);
	fsl_comp = to_fsl_qdma_comp(vdesc);

	memcpy(fsl_queue->virt_head++, fsl_comp->virt_addr,
					sizeof(struct fsl_qdma_format));
	if (fsl_queue->virt_head == fsl_queue->cq + fsl_queue->n_cq)
		fsl_queue->virt_head = fsl_queue->cq;

	list_add_tail(&fsl_comp->list, &fsl_queue->comp_used);
	barrier();
	reg = qdma_readl(fsl_chan->qdma, block + FSL_QDMA_BCQMR(fsl_queue->id));
	reg |= FSL_QDMA_BCQMR_EI;
	qdma_writel(fsl_chan->qdma, reg, block + FSL_QDMA_BCQMR(fsl_queue->id));
	fsl_chan->status = DMA_IN_PROGRESS;
}

static enum dma_status fsl_qdma_tx_status(struct dma_chan *chan,
		dma_cookie_t cookie, struct dma_tx_state *txstate)
{
	enum dma_status ret;

	ret = dma_cookie_status(chan, cookie, txstate);
	if (ret == DMA_COMPLETE || !txstate)
		return ret;

	return ret;
}

static void fsl_qdma_free_desc(struct virt_dma_desc *vdesc)
{
	struct fsl_qdma_comp *fsl_comp;
	struct fsl_qdma_queue *fsl_queue;
	unsigned long flags;

	fsl_comp = to_fsl_qdma_comp(vdesc);
	fsl_queue = fsl_comp->qchan->queue;

	spin_lock_irqsave(&fsl_queue->queue_lock, flags);
	list_add_tail(&fsl_comp->list, &fsl_queue->comp_free);
	spin_unlock_irqrestore(&fsl_queue->queue_lock, flags);
}

static void fsl_qdma_issue_pending(struct dma_chan *chan)
{
	struct fsl_qdma_chan *fsl_chan = to_fsl_qdma_chan(chan);
	struct fsl_qdma_queue *fsl_queue = fsl_chan->queue;
	unsigned long flags;

	spin_lock_irqsave(&fsl_queue->queue_lock, flags);
	spin_lock(&fsl_chan->vchan.lock);
	if (vchan_issue_pending(&fsl_chan->vchan))
		fsl_qdma_enqueue_desc(fsl_chan);
	spin_unlock(&fsl_chan->vchan.lock);
	spin_unlock_irqrestore(&fsl_queue->queue_lock, flags);
}

static int fsl_qdma_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct fsl_qdma_engine *fsl_qdma;
	struct fsl_qdma_chan *fsl_chan;
	struct resource *res;
	unsigned int len, chans, queues;
	int ret, i;

	ret = of_property_read_u32(np, "dma-channels", &chans);
	if (ret) {
		dev_err(&pdev->dev, "Can't get dma-channels.\n");
		return ret;
	}

	len = sizeof(*fsl_qdma) + sizeof(*fsl_chan) * chans;
	fsl_qdma = devm_kzalloc(&pdev->dev, len, GFP_KERNEL);
	if (!fsl_qdma)
		return -ENOMEM;

	ret = of_property_read_u32(np, "fsl,queues", &queues);
	if (ret) {
		dev_err(&pdev->dev, "Can't get queues.\n");
		return ret;
	}

	fsl_qdma->queue = fsl_qdma_alloc_queue_resources(pdev, queues);
	if (!fsl_qdma->queue)
		return -ENOMEM;

	fsl_qdma->status = fsl_qdma_prep_status_queue(pdev);
	if (!fsl_qdma->status)
		return -ENOMEM;

	fsl_qdma->n_chans = chans;
	fsl_qdma->n_queues = queues;
	mutex_init(&fsl_qdma->fsl_qdma_mutex);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	fsl_qdma->ctrl_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(fsl_qdma->ctrl_base))
		return PTR_ERR(fsl_qdma->ctrl_base);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	fsl_qdma->status_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(fsl_qdma->status_base))
		return PTR_ERR(fsl_qdma->status_base);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	fsl_qdma->block_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(fsl_qdma->block_base))
		return PTR_ERR(fsl_qdma->block_base);

	ret = fsl_qdma_irq_init(pdev, fsl_qdma);
	if (ret)
		return ret;

	fsl_qdma->feature = of_property_read_bool(np, "big-endian");
	INIT_LIST_HEAD(&fsl_qdma->dma_dev.channels);
	for (i = 0; i < fsl_qdma->n_chans; i++) {
		struct fsl_qdma_chan *fsl_chan = &fsl_qdma->chans[i];

		fsl_chan->qdma = fsl_qdma;
		fsl_chan->queue = fsl_qdma->queue + i % fsl_qdma->n_queues;
		fsl_chan->vchan.desc_free = fsl_qdma_free_desc;
		INIT_LIST_HEAD(&fsl_chan->qcomp);
		vchan_init(&fsl_chan->vchan, &fsl_qdma->dma_dev);
	}
	for (i = 0; i < fsl_qdma->n_queues; i++)
		fsl_qdma_pre_request_enqueue_desc(fsl_qdma->queue + i);

	dma_cap_set(DMA_MEMCPY, fsl_qdma->dma_dev.cap_mask);

	fsl_qdma->dma_dev.dev = &pdev->dev;
	fsl_qdma->dma_dev.device_free_chan_resources
		= fsl_qdma_free_chan_resources;
	fsl_qdma->dma_dev.device_tx_status = fsl_qdma_tx_status;
	fsl_qdma->dma_dev.device_prep_dma_memcpy = fsl_qdma_prep_memcpy;
	fsl_qdma->dma_dev.device_issue_pending = fsl_qdma_issue_pending;

	dma_set_mask(&pdev->dev, DMA_BIT_MASK(40));

	platform_set_drvdata(pdev, fsl_qdma);

	ret = dma_async_device_register(&fsl_qdma->dma_dev);
	if (ret) {
		dev_err(&pdev->dev, "Can't register NXP Layerscape qDMA engine.\n");
		return ret;
	}

	ret = fsl_qdma_reg_init(fsl_qdma);
	if (ret) {
		dev_err(&pdev->dev, "Can't Initialize the qDMA engine.\n");
		return ret;
	}

	return 0;
}

static void fsl_qdma_cleanup_vchan(struct dma_device *dmadev)
{
	struct fsl_qdma_chan *chan, *_chan;

	list_for_each_entry_safe(chan, _chan,
				&dmadev->channels, vchan.chan.device_node) {
		list_del(&chan->vchan.chan.device_node);
		tasklet_kill(&chan->vchan.task);
	}
}

static int fsl_qdma_remove(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct fsl_qdma_engine *fsl_qdma = platform_get_drvdata(pdev);
	struct fsl_qdma_queue *queue_temp;
	struct fsl_qdma_queue *status = fsl_qdma->status;
	struct fsl_qdma_comp *comp_temp, *_comp_temp;
	int i;

	fsl_qdma_irq_exit(pdev, fsl_qdma);
	fsl_qdma_cleanup_vchan(&fsl_qdma->dma_dev);
	of_dma_controller_free(np);
	dma_async_device_unregister(&fsl_qdma->dma_dev);

	/* Free descriptor areas */
	for (i = 0; i < fsl_qdma->n_queues; i++) {
		queue_temp = fsl_qdma->queue + i;
		list_for_each_entry_safe(comp_temp, _comp_temp,
					&queue_temp->comp_used,	list) {
			dma_pool_free(queue_temp->comp_pool,
					comp_temp->virt_addr,
					comp_temp->bus_addr);
			list_del(&comp_temp->list);
			kfree(comp_temp);
		}
		list_for_each_entry_safe(comp_temp, _comp_temp,
					&queue_temp->comp_free, list) {
			dma_pool_free(queue_temp->comp_pool,
					comp_temp->virt_addr,
					comp_temp->bus_addr);
			list_del(&comp_temp->list);
			kfree(comp_temp);
		}
		dma_free_coherent(&pdev->dev, sizeof(struct fsl_qdma_format) *
					queue_temp->n_cq, queue_temp->cq,
					queue_temp->bus_addr);
		dma_pool_destroy(queue_temp->comp_pool);
	}

	dma_free_coherent(&pdev->dev, sizeof(struct fsl_qdma_format) *
				status->n_cq, status->cq, status->bus_addr);
	return 0;
}

static const struct of_device_id fsl_qdma_dt_ids[] = {
	{ .compatible = "fsl,ls1021a-qdma", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, fsl_qdma_dt_ids);

static struct platform_driver fsl_qdma_driver = {
	.driver		= {
		.name	= "fsl-qdma",
		.of_match_table = fsl_qdma_dt_ids,
	},
	.probe          = fsl_qdma_probe,
	.remove		= fsl_qdma_remove,
};

module_platform_driver(fsl_qdma_driver);

MODULE_ALIAS("platform:fsl-qdma");
MODULE_DESCRIPTION("NXP Layerscape qDMA engine driver");
MODULE_LICENSE("GPL v2");
