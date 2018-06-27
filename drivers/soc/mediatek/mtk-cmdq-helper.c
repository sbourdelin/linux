// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 MediaTek Inc.
 *
 */

#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <linux/mailbox_controller.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#define CMDQ_ARG_A_WRITE_MASK	0xffff
#define CMDQ_WRITE_ENABLE_MASK	BIT(0)
#define CMDQ_EOC_IRQ_EN		BIT(0)
#define CMDQ_EOC_CMD		((u64)((CMDQ_CODE_EOC << CMDQ_OP_CODE_SHIFT)) \
				<< 32 | CMDQ_EOC_IRQ_EN)

int cmdq_pkt_realloc_cmd_buffer(struct cmdq_pkt *pkt, size_t size)
{
	void *new_buf;

	new_buf = krealloc(pkt->va_base, size, GFP_KERNEL | __GFP_ZERO);
	if (!new_buf)
		return -ENOMEM;
	pkt->va_base = new_buf;
	pkt->buf_size = size;

	return 0;
}
EXPORT_SYMBOL(cmdq_pkt_realloc_cmd_buffer);

struct cmdq_client *cmdq_mbox_create(struct device *dev, int index)
{
	struct cmdq_client *client;
	long err = 0;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return (struct cmdq_client *)-ENOMEM;

	client->client.dev = dev;
	client->client.tx_block = false;
	client->chan = mbox_request_channel(&client->client, index);

	if (IS_ERR(client->chan)) {
		dev_err(dev, "failed to request channel\n");
		err = PTR_ERR(client->chan);
		kfree(client);

		return (struct cmdq_client *)err;
	}

	return client;
}
EXPORT_SYMBOL(cmdq_mbox_create);

void cmdq_mbox_destroy(struct cmdq_client *client)
{
	mbox_free_channel(client->chan);
	kfree(client);
}
EXPORT_SYMBOL(cmdq_mbox_destroy);

int cmdq_pkt_create(struct cmdq_pkt **pkt_ptr)
{
	struct cmdq_pkt *pkt;
	int err;

	pkt = kzalloc(sizeof(*pkt), GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;
	err = cmdq_pkt_realloc_cmd_buffer(pkt, PAGE_SIZE);
	if (err < 0) {
		kfree(pkt);
		return err;
	}
	*pkt_ptr = pkt;

	return 0;
}
EXPORT_SYMBOL(cmdq_pkt_create);

void cmdq_pkt_destroy(struct cmdq_pkt *pkt)
{
	kfree(pkt->va_base);
	kfree(pkt);
}
EXPORT_SYMBOL(cmdq_pkt_destroy);

static bool cmdq_pkt_is_finalized(struct cmdq_pkt *pkt)
{
	u64 *expect_eoc;

	if (pkt->cmd_buf_size < CMDQ_INST_SIZE << 1)
		return false;

	expect_eoc = pkt->va_base + pkt->cmd_buf_size - (CMDQ_INST_SIZE << 1);
	if (*expect_eoc == CMDQ_EOC_CMD)
		return true;

	return false;
}

static int cmdq_pkt_append_command(struct cmdq_pkt *pkt, enum cmdq_code code,
				   u32 arg_a, u32 arg_b)
{
	u64 *cmd_ptr;
	int err;

	if (WARN_ON(cmdq_pkt_is_finalized(pkt)))
		return -EBUSY;
	if (unlikely(pkt->cmd_buf_size + CMDQ_INST_SIZE > pkt->buf_size)) {
		err = cmdq_pkt_realloc_cmd_buffer(pkt, pkt->buf_size << 1);
		if (err < 0)
			return err;
	}
	cmd_ptr = pkt->va_base + pkt->cmd_buf_size;
	(*cmd_ptr) = (u64)((code << CMDQ_OP_CODE_SHIFT) | arg_a) << 32 | arg_b;
	pkt->cmd_buf_size += CMDQ_INST_SIZE;

	return 0;
}

int cmdq_pkt_write(struct cmdq_pkt *pkt, u32 value, u32 subsys, u32 offset)
{
	u32 arg_a = (offset & CMDQ_ARG_A_WRITE_MASK) |
		    (subsys << CMDQ_SUBSYS_SHIFT);

	return cmdq_pkt_append_command(pkt, CMDQ_CODE_WRITE, arg_a, value);
}
EXPORT_SYMBOL(cmdq_pkt_write);

int cmdq_pkt_write_mask(struct cmdq_pkt *pkt, u32 value,
			u32 subsys, u32 offset, u32 mask)
{
	u32 offset_mask = offset;
	int err;

	if (mask != 0xffffffff) {
		err = cmdq_pkt_append_command(pkt, CMDQ_CODE_MASK, 0, ~mask);
		if (err < 0)
			return err;
		offset_mask |= CMDQ_WRITE_ENABLE_MASK;
	}

	return cmdq_pkt_write(pkt, value, subsys, offset_mask);
}
EXPORT_SYMBOL(cmdq_pkt_write_mask);

int cmdq_pkt_wfe(struct cmdq_pkt *pkt, u32 event)
{
	u32 arg_b;

	if (event >= CMDQ_MAX_EVENT || event < 0)
		return -EINVAL;

	/*
	 * WFE arg_b
	 * bit 0-11: wait value
	 * bit 15: 1 - wait, 0 - no wait
	 * bit 16-27: update value
	 * bit 31: 1 - update, 0 - no update
	 */
	arg_b = CMDQ_WFE_UPDATE | CMDQ_WFE_WAIT | CMDQ_WFE_WAIT_VALUE;

	return cmdq_pkt_append_command(pkt, CMDQ_CODE_WFE, event, arg_b);
}
EXPORT_SYMBOL(cmdq_pkt_wfe);

int cmdq_pkt_clear_event(struct cmdq_pkt *pkt, u32 event)
{
	if (event >= CMDQ_MAX_EVENT || event < 0)
		return -EINVAL;

	return cmdq_pkt_append_command(pkt, CMDQ_CODE_WFE, event,
				       CMDQ_WFE_UPDATE);
}
EXPORT_SYMBOL(cmdq_pkt_clear_event);

static int cmdq_pkt_finalize(struct cmdq_pkt *pkt)
{
	int err;

	if (cmdq_pkt_is_finalized(pkt))
		return 0;

	/* insert EOC and generate IRQ for each command iteration */
	err = cmdq_pkt_append_command(pkt, CMDQ_CODE_EOC, 0, CMDQ_EOC_IRQ_EN);
	if (err < 0)
		return err;

	/* JUMP to end */
	err = cmdq_pkt_append_command(pkt, CMDQ_CODE_JUMP, 0, CMDQ_JUMP_PASS);
	if (err < 0)
		return err;

	return 0;
}

int cmdq_pkt_flush_async(struct cmdq_client *client, struct cmdq_pkt *pkt,
			 cmdq_async_flush_cb cb, void *data)
{
	int err;
	struct device *dev;
	dma_addr_t dma_addr;

	err = cmdq_pkt_finalize(pkt);
	if (err < 0)
		return err;

	dev = client->chan->mbox->dev;
	dma_addr = dma_map_single(dev, pkt->va_base, pkt->cmd_buf_size,
		DMA_TO_DEVICE);
	if (dma_mapping_error(dev, dma_addr)) {
		dev_err(client->chan->mbox->dev, "dma map failed\n");
		return -ENOMEM;
	}

	pkt->pa_base = dma_addr;
	pkt->cb.cb = cb;
	pkt->cb.data = data;

	mbox_send_message(client->chan, pkt);
	/* We can send next packet immediately, so just call txdone. */
	mbox_client_txdone(client->chan, 0);

	return 0;
}
EXPORT_SYMBOL(cmdq_pkt_flush_async);

struct cmdq_flush_completion {
	struct completion cmplt;
	bool err;
};

static void cmdq_pkt_flush_cb(struct cmdq_cb_data data)
{
	struct cmdq_flush_completion *cmplt = data.data;

	cmplt->err = data.err;
	complete(&cmplt->cmplt);
}

int cmdq_pkt_flush(struct cmdq_client *client, struct cmdq_pkt *pkt)
{
	struct cmdq_flush_completion cmplt;
	int err;

	init_completion(&cmplt.cmplt);
	err = cmdq_pkt_flush_async(client, pkt, cmdq_pkt_flush_cb, &cmplt);
	if (err < 0)
		return err;
	wait_for_completion(&cmplt.cmplt);

	return cmplt.err ? -EFAULT : 0;
}
EXPORT_SYMBOL(cmdq_pkt_flush);
