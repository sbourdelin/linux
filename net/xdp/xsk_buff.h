#ifndef XSK_BUFF_H_
#define XSK_BUFF_H_

#include <linux/types.h> /* dma_addr_t */
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>

#include "xsk.h"

struct xsk_buff {
	void *data;
	dma_addr_t dma;
	unsigned int len; /* XXX really needed? */
	unsigned int id;
	unsigned int offset;
	struct xsk_buff *next;
};

/* Rx: data + umem->data_headroom + XDP_PACKET_HEADROOM */
/* Tx: data + desc->offset */

struct xsk_buff_info {
	struct xsk_umem *umem;
	struct device *dev;
	enum dma_data_direction dir;
	unsigned long attrs;
	unsigned int rx_headroom;
	unsigned int buff_len;
	unsigned int nbuffs;
	struct xsk_buff buffs[0];

};

static inline int xsk_buff_dma_map(struct xsk_buff_info *info,
				   struct device *dev,
				   enum dma_data_direction dir,
				   unsigned long attrs)
{
	struct xsk_buff *b;
	unsigned int i, j;
	dma_addr_t dma;

	if (info->dev)
		return -1; /* Already mapped */

	for (i = 0; i < info->nbuffs; i++) {
		b = &info->buffs[i];
		dma = dma_map_single_attrs(dev, b->data, b->len, dir, attrs);
		if (dma_mapping_error(dev, dma))
			goto out_unmap;

		b->dma = dma;
	}

	info->dev = dev;
	info->dir = dir;
	info->attrs = attrs;

	return 0;

out_unmap:
	for (j = 0; j < i; j++) {
		b = &info->buffs[i];
		dma_unmap_single_attrs(info->dev, b->dma, b->len,
				       info->dir, info->attrs);
		b->dma = 0;
	}

	return -1;
}

static inline void xsk_buff_dma_unmap(struct xsk_buff_info *info)
{
	struct xsk_buff *b;
	unsigned int i;

	if (!info->dev)
		return; /* Nothing mapped! */

	for (i = 0; i < info->nbuffs; i++) {
		b = &info->buffs[i];
		dma_unmap_single_attrs(info->dev, b->dma, b->len,
				       info->dir, info->attrs);
		b->dma = 0;
	}

	info->dev = NULL;
	info->dir = DMA_NONE;
	info->attrs = 0;
}

/* --- */

static inline struct xsk_buff *xsk_buff_info_get_buff(
	struct xsk_buff_info *info,
	u32 id)
{
	/* XXX remove */
	if (id >= info->nbuffs) {
		WARN(1, "%s bad id\n", __func__);
		return NULL;
	}

	return &info->buffs[id];
}

static inline unsigned int xsk_buff_info_get_rx_headroom(
	struct xsk_buff_info *info)
{
	return info->rx_headroom;
}

static inline unsigned int xsk_buff_info_get_buff_len(
	struct xsk_buff_info *info)
{
	return info->buff_len;
}

static inline struct xsk_buff_info *xsk_buff_info_create(struct xsk_umem *umem)
{
	struct xsk_buff_info *buff_info;
	unsigned int id = 0;
	void *data, *end;
	u32 i;

	buff_info = vzalloc(sizeof(*buff_info) +
			    sizeof(struct xsk_buff) * umem->nframes);
	if (!buff_info)
		return NULL;

	buff_info->umem = umem;
	buff_info->rx_headroom = umem->data_headroom;
	buff_info->buff_len = umem->frame_size;
	buff_info->nbuffs = umem->nframes;

	for (i = 0; i < umem->npgs; i++) {
		data = page_address(umem->pgs[i]);
		end = data + PAGE_SIZE;
		while (data < end) {
			struct xsk_buff *buff = &buff_info->buffs[id];

			buff->data = data;
			buff->len = buff_info->buff_len;
			buff->id = id;
			buff->offset = buff_info->rx_headroom;

			data += buff_info->buff_len;
			id++;
		}
	}

	return buff_info;
}

static inline void xsk_buff_info_destroy(struct xsk_buff_info *info)
{
	xsk_buff_dma_unmap(info);
	vfree(info);
}

#endif /* XSK_BUFF_H_ */
