/*
 * Cryptographic API.
 *
 * Support for Samsung S5PV210 and Exynos HW acceleration.
 *
 * Copyright (C) 2011 NetUP Inc. All rights reserved.
 * Copyright (c) 2017 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * Hash part based on omap-sham.c driver.
 */

#include <linux/clk.h>
#include <linux/crypto.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>

#include <crypto/ctr.h>
#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <crypto/scatterwalk.h>

#include <crypto/hash.h>
#include <crypto/md5.h>
#include <crypto/sha.h>
#include <crypto/internal/hash.h>

#define _SBF(s, v)                      ((v) << (s))

#ifdef DEBUG

static int flow_debug_logging;
static int flow_debug_dump;

/* from crypto/bcm/util.h */
#define FLOW_LOG(...)				\
	do {					\
		if (flow_debug_logging) {	\
			printk(__VA_ARGS__);	\
		}				\
	} while (0)
#define FLOW_DUMP(msg, var, var_len)		\
	do {					\
		if (flow_debug_dump) {		\
			print_hex_dump(KERN_ALERT, msg, DUMP_PREFIX_NONE, \
					16, 1, var, var_len, false); \
		}				\
	} while (0)
#else /* !DEBUG */

#define FLOW_LOG(...)			do {} while (0)
#define FLOW_DUMP(msg, var, var_len)	do {} while (0)

#endif /* DEBUG */

/* Feed control registers */
#define SSS_REG_FCINTSTAT               0x0000
#define SSS_FCINTSTAT_HPARTINT		BIT(7)
#define SSS_FCINTSTAT_HDONEINT		BIT(5)
#define SSS_FCINTSTAT_BRDMAINT          BIT(3)
#define SSS_FCINTSTAT_BTDMAINT          BIT(2)
#define SSS_FCINTSTAT_HRDMAINT          BIT(1)
#define SSS_FCINTSTAT_PKDMAINT          BIT(0)

#define SSS_REG_FCINTENSET              0x0004
#define SSS_FCINTENSET_HPARTINTENSET	BIT(7)
#define SSS_FCINTENSET_HDONEINTENSET	BIT(5)
#define SSS_FCINTENSET_BRDMAINTENSET    BIT(3)
#define SSS_FCINTENSET_BTDMAINTENSET    BIT(2)
#define SSS_FCINTENSET_HRDMAINTENSET    BIT(1)
#define SSS_FCINTENSET_PKDMAINTENSET    BIT(0)

#define SSS_REG_FCINTENCLR              0x0008
#define SSS_FCINTENCLR_HPARTINTENCLR	BIT(7)
#define SSS_FCINTENCLR_HDONEINTENCLR	BIT(5)
#define SSS_FCINTENCLR_BRDMAINTENCLR    BIT(3)
#define SSS_FCINTENCLR_BTDMAINTENCLR    BIT(2)
#define SSS_FCINTENCLR_HRDMAINTENCLR    BIT(1)
#define SSS_FCINTENCLR_PKDMAINTENCLR    BIT(0)

#define SSS_REG_FCINTPEND               0x000C
#define SSS_FCINTPEND_HPARTINTP		BIT(7)
#define SSS_FCINTPEND_HDONEINTP		BIT(5)
#define SSS_FCINTPEND_BRDMAINTP         BIT(3)
#define SSS_FCINTPEND_BTDMAINTP         BIT(2)
#define SSS_FCINTPEND_HRDMAINTP         BIT(1)
#define SSS_FCINTPEND_PKDMAINTP         BIT(0)

#define SSS_REG_FCFIFOSTAT              0x0010
#define SSS_FCFIFOSTAT_BRFIFOFUL        BIT(7)
#define SSS_FCFIFOSTAT_BRFIFOEMP        BIT(6)
#define SSS_FCFIFOSTAT_BTFIFOFUL        BIT(5)
#define SSS_FCFIFOSTAT_BTFIFOEMP        BIT(4)
#define SSS_FCFIFOSTAT_HRFIFOFUL        BIT(3)
#define SSS_FCFIFOSTAT_HRFIFOEMP        BIT(2)
#define SSS_FCFIFOSTAT_PKFIFOFUL        BIT(1)
#define SSS_FCFIFOSTAT_PKFIFOEMP        BIT(0)

#define SSS_REG_FCFIFOCTRL              0x0014
#define SSS_FCFIFOCTRL_DESSEL           BIT(2)
#define SSS_HASHIN_INDEPENDENT          _SBF(0, 0x00)
#define SSS_HASHIN_CIPHER_INPUT         _SBF(0, 0x01)
#define SSS_HASHIN_CIPHER_OUTPUT        _SBF(0, 0x02)
#define SSS_HASHIN_MASK			_SBF(0, 0x03)

#define SSS_REG_FCBRDMAS                0x0020
#define SSS_REG_FCBRDMAL                0x0024
#define SSS_REG_FCBRDMAC                0x0028
#define SSS_FCBRDMAC_BYTESWAP           BIT(1)
#define SSS_FCBRDMAC_FLUSH              BIT(0)

#define SSS_REG_FCBTDMAS                0x0030
#define SSS_REG_FCBTDMAL                0x0034
#define SSS_REG_FCBTDMAC                0x0038
#define SSS_FCBTDMAC_BYTESWAP           BIT(1)
#define SSS_FCBTDMAC_FLUSH              BIT(0)

#define SSS_REG_FCHRDMAS                0x0040
#define SSS_REG_FCHRDMAL                0x0044
#define SSS_REG_FCHRDMAC                0x0048
#define SSS_FCHRDMAC_BYTESWAP           BIT(1)
#define SSS_FCHRDMAC_FLUSH              BIT(0)

#define SSS_REG_FCPKDMAS                0x0050
#define SSS_REG_FCPKDMAL                0x0054
#define SSS_REG_FCPKDMAC                0x0058
#define SSS_FCPKDMAC_BYTESWAP           BIT(3)
#define SSS_FCPKDMAC_DESCEND            BIT(2)
#define SSS_FCPKDMAC_TRANSMIT           BIT(1)
#define SSS_FCPKDMAC_FLUSH              BIT(0)

#define SSS_REG_FCPKDMAO                0x005C

/* AES registers */
#define SSS_REG_AES_CONTROL		0x00
#define SSS_AES_BYTESWAP_DI             BIT(11)
#define SSS_AES_BYTESWAP_DO             BIT(10)
#define SSS_AES_BYTESWAP_IV             BIT(9)
#define SSS_AES_BYTESWAP_CNT            BIT(8)
#define SSS_AES_BYTESWAP_KEY            BIT(7)
#define SSS_AES_KEY_CHANGE_MODE         BIT(6)
#define SSS_AES_KEY_SIZE_128            _SBF(4, 0x00)
#define SSS_AES_KEY_SIZE_192            _SBF(4, 0x01)
#define SSS_AES_KEY_SIZE_256            _SBF(4, 0x02)
#define SSS_AES_FIFO_MODE               BIT(3)
#define SSS_AES_CHAIN_MODE_ECB          _SBF(1, 0x00)
#define SSS_AES_CHAIN_MODE_CBC          _SBF(1, 0x01)
#define SSS_AES_CHAIN_MODE_CTR          _SBF(1, 0x02)
#define SSS_AES_MODE_DECRYPT            BIT(0)

#define SSS_REG_AES_STATUS		0x04
#define SSS_AES_BUSY                    BIT(2)
#define SSS_AES_INPUT_READY             BIT(1)
#define SSS_AES_OUTPUT_READY            BIT(0)

#define SSS_REG_AES_IN_DATA(s)		(0x10 + (s << 2))
#define SSS_REG_AES_OUT_DATA(s)		(0x20 + (s << 2))
#define SSS_REG_AES_IV_DATA(s)		(0x30 + (s << 2))
#define SSS_REG_AES_CNT_DATA(s)		(0x40 + (s << 2))
#define SSS_REG_AES_KEY_DATA(s)		(0x80 + (s << 2))

#define SSS_REG(dev, reg)               ((dev)->ioaddr + (SSS_REG_##reg))
#define SSS_READ(dev, reg)              __raw_readl(SSS_REG(dev, reg))
#define SSS_WRITE(dev, reg, val)        __raw_writel((val), SSS_REG(dev, reg))

#define SSS_AES_REG(dev, reg)           ((dev)->aes_ioaddr + SSS_REG_##reg)
#define SSS_AES_WRITE(dev, reg, val)    __raw_writel((val), \
						SSS_AES_REG(dev, reg))

/* HW engine modes */
#define FLAGS_AES_DECRYPT               BIT(0)
#define FLAGS_AES_MODE_MASK             _SBF(1, 0x03)
#define FLAGS_AES_CBC                   _SBF(1, 0x01)
#define FLAGS_AES_CTR                   _SBF(1, 0x02)

#define AES_KEY_LEN         16
#define CRYPTO_QUEUE_LEN    1

/* HASH registers */
#define SSS_REG_HASH_CTRL		0x00

#define SSS_HASH_USER_IV_EN		BIT(5)
#define SSS_HASH_INIT_BIT		BIT(4)
#define SSS_HASH_ENGINE_SHA1		_SBF(1, 0x00)
#define SSS_HASH_ENGINE_MD5		_SBF(1, 0x01)
#define SSS_HASH_ENGINE_SHA256		_SBF(1, 0x02)

#define SSS_HASH_ENGINE_MASK		_SBF(1, 0x03)

#define SSS_REG_HASH_CTRL_PAUSE		0x04

#define SSS_HASH_PAUSE			BIT(0)

#define SSS_REG_HASH_CTRL_FIFO		0x08

#define SSS_HASH_FIFO_MODE_DMA		BIT(0)
#define SSS_HASH_FIFO_MODE_CPU          0

#define SSS_REG_HASH_CTRL_SWAP		0x0c

#define SSS_HASH_BYTESWAP_DI		BIT(3)
#define SSS_HASH_BYTESWAP_DO		BIT(2)
#define SSS_HASH_BYTESWAP_IV		BIT(1)
#define SSS_HASH_BYTESWAP_KEY		BIT(0)

#define SSS_REG_HASH_STATUS		0x10

#define SSS_HASH_STATUS_MSG_DONE	BIT(6)
#define SSS_HASH_STATUS_PARTIAL_DONE	BIT(4)
#define SSS_HASH_STATUS_BUFFER_READY	BIT(0)

#define SSS_REG_HASH_MSG_SIZE_LOW	0x20
#define SSS_REG_HASH_MSG_SIZE_HIGH	0x24

#define SSS_REG_HASH_PRE_MSG_SIZE_LOW	0x28
#define SSS_REG_HASH_PRE_MSG_SIZE_HIGH	0x2c

#define SSS_REG_TYPE			u32
#define HASH_MAX_REG			16
#define HASH_REG_SIZEOF			sizeof(SSS_REG_TYPE)

#define HASH_BLOCK_SIZE			(HASH_MAX_REG*HASH_REG_SIZEOF)

#define HASH_MD5_MAX_REG		(MD5_DIGEST_SIZE / HASH_REG_SIZEOF)
#define HASH_SHA1_MAX_REG		(SHA1_DIGEST_SIZE / HASH_REG_SIZEOF)
#define HASH_SHA256_MAX_REG		(SHA256_DIGEST_SIZE / HASH_REG_SIZEOF)

#define SSS_REG_HASH_IV(s)		(0xB0 + ((s) << 2))
#define SSS_REG_HASH_OUT(s)		(0x100 + ((s) << 2))

#define DEFAULT_TIMEOUT_INTERVAL	HZ

#define DEFAULT_AUTOSUSPEND_DELAY	1000

/* HASH flags */
#define HASH_FLAGS_BUSY		0
#define HASH_FLAGS_FINAL	1
#define HASH_FLAGS_DMA_ACTIVE	2
#define HASH_FLAGS_OUTPUT_READY	3
#define HASH_FLAGS_INIT		4
#define HASH_FLAGS_DMA_READY	6

#define HASH_FLAGS_SGS_COPIED	9
#define HASH_FLAGS_SGS_ALLOCED	10
/* HASH context flags */
#define HASH_FLAGS_FINUP	16
#define HASH_FLAGS_ERROR	17

#define HASH_FLAGS_MODE_MD5	18
#define HASH_FLAGS_MODE_SHA1	19
#define HASH_FLAGS_MODE_SHA256	20

#define HASH_FLAGS_MODE_MASK	(BIT(18) | BIT(19) | BIT(20))
/* HASH op codes */
#define HASH_OP_UPDATE		1
#define HASH_OP_FINAL		2

/* HASH HW constants */
#define HASH_ALIGN_MASK		(HASH_BLOCK_SIZE-1)

#define BUFLEN			HASH_BLOCK_SIZE

#define SSS_DMA_ALIGN		16
#define SSS_ALIGNED		__attribute__((aligned(SSS_DMA_ALIGN)))
#define SSS_DMA_ALIGN_MASK	(SSS_DMA_ALIGN-1)

/* HASH queue constant */
#define SSS_HASH_QUEUE_LENGTH	10

/**
 * struct sss_hash_algs_info - platform specific SSS HASH algorithms
 * @algs_list:	array of transformations (algorithms)
 * @size:	size
 * @registered:	counter used at probe/remove
 *
 * Specifies platform specific information about hash algorithms
 * of SSS module.
 */
struct sss_hash_algs_info {
	struct ahash_alg	*algs_list;
	unsigned int		size;
	unsigned int		registered;
};

/**
 * struct samsung_aes_variant - platform specific SSS driver data
 * @aes_offset: AES register offset from SSS module's base.
 * @hash_offset: HASH register offset from SSS module's base.
 *
 * @hash_algs_info: HASH transformations provided by SS module
 * @hash_algs_size: size of hash_algs_info
 *
 * Specifies platform specific configuration of SSS module.
 * Note: A structure for driver specific platform data is used for future
 * expansion of its usage.
 */
struct samsung_aes_variant {
	unsigned int			aes_offset;
	unsigned int			hash_offset;

	struct sss_hash_algs_info	*hash_algs_info;
	unsigned int			hash_algs_size;
};

struct s5p_aes_reqctx {
	unsigned long			mode;
};

struct s5p_aes_ctx {
	struct s5p_aes_dev		*dev;

	uint8_t				aes_key[AES_MAX_KEY_SIZE];
	uint8_t				nonce[CTR_RFC3686_NONCE_SIZE];
	int				keylen;
};

/**
 * struct s5p_aes_dev - Crypto device state container
 * @dev:	Associated device
 * @clk:	Clock for accessing hardware
 * @ioaddr:	Mapped IO memory region
 * @aes_ioaddr:	Per-variant offset for AES block IO memory
 * @irq_fc:	Feed control interrupt line
 * @req:	Crypto request currently handled by the device
 * @ctx:	Configuration for currently handled crypto request
 * @sg_src:	Scatter list with source data for currently handled block
 *		in device.  This is DMA-mapped into device.
 * @sg_dst:	Scatter list with destination data for currently handled block
 *		in device. This is DMA-mapped into device.
 * @sg_src_cpy:	In case of unaligned access, copied scatter list
 *		with source data.
 * @sg_dst_cpy:	In case of unaligned access, copied scatter list
 *		with destination data.
 * @tasklet:	New request scheduling job
 * @queue:	Crypto queue
 * @busy:	Indicates whether the device is currently handling some request
 *		thus it uses some of the fields from this state, like:
 *		req, ctx, sg_src/dst (and copies).  This essentially
 *		protects against concurrent access to these fields.
 * @lock:	Lock for protecting both access to device hardware registers
 *		and fields related to current request (including the busy
 *		field).
 * @res:	Resources for hash.
 * @io_hash_base: Per-variant offset for HASH block IO memory.
 * @hash_lock:	Lock for protecting hash_req and other HASH variables.
 * @hash_err:	Error flags for current HASH op.
 * @hash_tasklet: New HASH request scheduling job.
 * @xmit_buf:	Buffer for current HASH request transfer into SSS block.
 * @hash_flags:	Flags for current HASH op.
 * @hash_queue:	Async hash queue.
 * @hash_req:	Current request sending to SSS HASH block.
 * @hash_sg_iter: Scatterlist transferred through DMA into SSS HASH block.
 * @hash_sg_cnt: Counter for hash_sg_iter.
 *
 * @pdata:	Per-variant algorithms for HASH ops.
 */
struct s5p_aes_dev {
	struct device			*dev;
	struct clk			*clk;
	void __iomem			*ioaddr;
	void __iomem			*aes_ioaddr;
	int				irq_fc;

	struct ablkcipher_request	*req;
	struct s5p_aes_ctx		*ctx;
	struct scatterlist		*sg_src;
	struct scatterlist		*sg_dst;

	struct scatterlist		*sg_src_cpy;
	struct scatterlist		*sg_dst_cpy;

	struct tasklet_struct		tasklet;
	struct crypto_queue		queue;
	bool				busy;
	spinlock_t			lock;

	struct resource			*res;
	void __iomem			*io_hash_base;

	spinlock_t			hash_lock;
	int				hash_err;
	struct tasklet_struct		hash_tasklet;
	u8				xmit_buf[BUFLEN] SSS_ALIGNED;

	unsigned long			hash_flags;
	struct crypto_queue		hash_queue;
	struct ahash_request		*hash_req;
	struct scatterlist		*hash_sg_iter;
	int				hash_sg_cnt;

	struct samsung_aes_variant	*pdata;
};

/**
 * struct s5p_hash_reqctx - HASH request context
 * @dev:	Associated device
 * @flags:	Bits for current HASH request
 * @op:		Current request operation (OP_UPDATE or UP_FINAL)
 * @digcnt:	Number of bytes processed by HW (without buffer[] ones)
 * @digest:	Digest message or IV for partial result
 * @bufcnt:	Number of bytes holded in buffer[]
 * @buflen:	Max length of the input data buffer
 * @nregs:	Number of HW registers for digest or IV read/write.
 * @engine:	Flags for setting HASH SSS block.
 * @sg:		sg for DMA transfer.
 * @sg_len:	Length of sg for DMA transfer.
 * @sgl[]:	sg for joining buffer and req->src scatterlist.
 * @skip:	Skip offset in req->src for current op.
 * @total:	Total number of bytes for current request.
 * @buffer[]:	For byte(s) from end of req->src in UPDATE op.
 */
struct s5p_hash_reqctx {
	struct s5p_aes_dev	*dd;
	unsigned long		flags;
	int			op;

	u64			digcnt;
	u8			digest[SHA256_DIGEST_SIZE] SSS_ALIGNED;
	u32			bufcnt;
	u32			buflen;

	int			nregs; /* digest_size / sizeof(reg) */
	u32			engine;

	struct scatterlist	*sg;
	int			sg_len;
	struct scatterlist	sgl[2];
	int			skip;	/* skip offset in req->src sg */
	unsigned int		total;	/* total request */

	u8			buffer[0] SSS_ALIGNED;
};

/**
 * struct s5p_hash_ctx - HASH transformation context
 * @dd:		Associated device
 * @flags:	Bits for algorithm HASH.
 * @fallback:	Software transformation for zero message or size < BUFLEN.
 */
struct s5p_hash_ctx {
	struct s5p_aes_dev	*dd;
	unsigned long		flags;
	struct crypto_shash	*fallback;
};

static struct samsung_aes_variant s5p_aes_data = {
	.aes_offset	= 0x4000,
	.hash_offset	= 0x6000,
	.hash_algs_size	= 0,
};

static struct samsung_aes_variant exynos_aes_data = {
	.aes_offset		= 0x200,
	.hash_offset		= 0x400,
};

static const struct of_device_id s5p_sss_dt_match[] = {
	{
		.compatible = "samsung,s5pv210-secss",
		.data = &s5p_aes_data,
	},
	{
		.compatible = "samsung,exynos4210-secss",
		.data = &exynos_aes_data,
	},
	{ },
};
MODULE_DEVICE_TABLE(of, s5p_sss_dt_match);

static inline struct samsung_aes_variant *find_s5p_sss_version
				   (struct platform_device *pdev)
{
	if (IS_ENABLED(CONFIG_OF) && (pdev->dev.of_node)) {
		const struct of_device_id *match;

		match = of_match_node(s5p_sss_dt_match,
					pdev->dev.of_node);
		return (struct samsung_aes_variant *)match->data;
	}
	return (struct samsung_aes_variant *)
			platform_get_device_id(pdev)->driver_data;
}

static struct s5p_aes_dev *s5p_dev;

static void s5p_set_dma_indata(struct s5p_aes_dev *dev, struct scatterlist *sg)
{
	SSS_WRITE(dev, FCBRDMAS, sg_dma_address(sg));
	SSS_WRITE(dev, FCBRDMAL, sg_dma_len(sg));
}

static void s5p_set_dma_outdata(struct s5p_aes_dev *dev, struct scatterlist *sg)
{
	SSS_WRITE(dev, FCBTDMAS, sg_dma_address(sg));
	SSS_WRITE(dev, FCBTDMAL, sg_dma_len(sg));
}

static void s5p_free_sg_cpy(struct s5p_aes_dev *dev, struct scatterlist **sg)
{
	int len;

	if (!*sg)
		return;

	len = ALIGN(dev->req->nbytes, AES_BLOCK_SIZE);
	free_pages((unsigned long)sg_virt(*sg), get_order(len));

	kfree(*sg);
	*sg = NULL;
}

static void s5p_sg_copy_buf(void *buf, struct scatterlist *sg,
			    unsigned int nbytes, int out)
{
	struct scatter_walk walk;

	if (!nbytes)
		return;

	scatterwalk_start(&walk, sg);
	scatterwalk_copychunks(buf, &walk, nbytes, out);
	scatterwalk_done(&walk, out, 0);
}

static void s5p_sg_done(struct s5p_aes_dev *dev)
{
	if (dev->sg_dst_cpy) {
		dev_dbg(dev->dev,
			"Copying %d bytes of output data back to original place\n",
			dev->req->nbytes);
		s5p_sg_copy_buf(sg_virt(dev->sg_dst_cpy), dev->req->dst,
				dev->req->nbytes, 1);
	}
	s5p_free_sg_cpy(dev, &dev->sg_src_cpy);
	s5p_free_sg_cpy(dev, &dev->sg_dst_cpy);
}

/* Calls the completion. Cannot be called with dev->lock hold. */
static void s5p_aes_complete(struct s5p_aes_dev *dev, int err)
{
	dev->req->base.complete(&dev->req->base, err);
}

static void s5p_unset_outdata(struct s5p_aes_dev *dev)
{
	dma_unmap_sg(dev->dev, dev->sg_dst, 1, DMA_FROM_DEVICE);
}

static void s5p_unset_indata(struct s5p_aes_dev *dev)
{
	dma_unmap_sg(dev->dev, dev->sg_src, 1, DMA_TO_DEVICE);
}

static int s5p_make_sg_cpy(struct s5p_aes_dev *dev, struct scatterlist *src,
			    struct scatterlist **dst)
{
	void *pages;
	int len;

	*dst = kmalloc(sizeof(**dst), GFP_ATOMIC);
	if (!*dst)
		return -ENOMEM;

	len = ALIGN(dev->req->nbytes, AES_BLOCK_SIZE);
	pages = (void *)__get_free_pages(GFP_ATOMIC, get_order(len));
	if (!pages) {
		kfree(*dst);
		*dst = NULL;
		return -ENOMEM;
	}

	s5p_sg_copy_buf(pages, src, dev->req->nbytes, 0);

	sg_init_table(*dst, 1);
	sg_set_buf(*dst, pages, len);

	return 0;
}

static int s5p_set_outdata(struct s5p_aes_dev *dev, struct scatterlist *sg)
{
	int err;

	if (!sg->length) {
		err = -EINVAL;
		goto exit;
	}

	err = dma_map_sg(dev->dev, sg, 1, DMA_FROM_DEVICE);
	if (!err) {
		err = -ENOMEM;
		goto exit;
	}

	dev->sg_dst = sg;
	err = 0;

exit:
	return err;
}

static int s5p_set_indata(struct s5p_aes_dev *dev, struct scatterlist *sg)
{
	int err;

	if (!sg->length) {
		err = -EINVAL;
		goto exit;
	}

	err = dma_map_sg(dev->dev, sg, 1, DMA_TO_DEVICE);
	if (!err) {
		err = -ENOMEM;
		goto exit;
	}

	dev->sg_src = sg;
	err = 0;

exit:
	return err;
}

/*
 * Returns -ERRNO on error (mapping of new data failed).
 * On success returns:
 *  - 0 if there is no more data,
 *  - 1 if new transmitting (output) data is ready and its address+length
 *     have to be written to device (by calling s5p_set_dma_outdata()).
 */
static int s5p_aes_tx(struct s5p_aes_dev *dev)
{
	int ret = 0;

	s5p_unset_outdata(dev);

	if (!sg_is_last(dev->sg_dst)) {
		ret = s5p_set_outdata(dev, sg_next(dev->sg_dst));
		if (!ret)
			ret = 1;
	}

	return ret;
}

/*
 * Returns -ERRNO on error (mapping of new data failed).
 * On success returns:
 *  - 0 if there is no more data,
 *  - 1 if new receiving (input) data is ready and its address+length
 *     have to be written to device (by calling s5p_set_dma_indata()).
 */
static int s5p_aes_rx(struct s5p_aes_dev *dev/*, bool *set_dma*/)
{
	int ret = 0;

	s5p_unset_indata(dev);

	if (!sg_is_last(dev->sg_src)) {
		ret = s5p_set_indata(dev, sg_next(dev->sg_src));
		if (!ret)
			ret = 1;
	}

	return ret;
}

static inline u32 s5p_hash_read(struct s5p_aes_dev *dd, u32 offset)
{
	return __raw_readl(dd->io_hash_base + offset);
}

static inline void s5p_hash_write(struct s5p_aes_dev *dd,
				  u32 offset, u32 value)
{
	__raw_writel(value, dd->io_hash_base + offset);
}

static inline void s5p_hash_write_mask(struct s5p_aes_dev *dd, u32 address,
				       u32 value, u32 mask)
{
	u32 val;

	val = s5p_hash_read(dd, address);
	val &= ~mask;
	val |= value;
	s5p_hash_write(dd, address, val);
}

/**
 * s5p_set_dma_hashdata - start DMA with sg
 * @dev:	device
 * @sg:		scatterlist ready to DMA transmit
 *
 * decrement sg counter
 * write addr and len into HASH regs
 *
 * DMA starts after writing length
 */
static void s5p_set_dma_hashdata(struct s5p_aes_dev *dev,
				 struct scatterlist *sg)
{
	FLOW_LOG("sg_cnt=%d, sg=%p len=%d", dev->hash_sg_cnt, sg, sg->length);
	dev->hash_sg_cnt--;
	WARN_ON(dev->hash_sg_cnt < 0);
	WARN_ON(sg_dma_len(sg) <= 0);
	SSS_WRITE(dev, FCHRDMAS, sg_dma_address(sg));
	SSS_WRITE(dev, FCHRDMAL, sg_dma_len(sg)); /* DMA starts */
}

/**
 * s5p_hash_rx - get next hash_sg_iter
 * @dev:	device
 *
 * Return:
 * 2	if there is no more data,
 * 1	if new receiving (input) data is ready and can be written to
 *	device
 */
static int s5p_hash_rx(struct s5p_aes_dev *dev)
{
	int ret = 2;

	FLOW_LOG("hash_rx sg_cnt=%d", dev->hash_sg_cnt);
	if (dev->hash_sg_cnt > 0) {
		dev->hash_sg_iter = sg_next(dev->hash_sg_iter);
		ret = 1;
	} else {
		set_bit(HASH_FLAGS_DMA_READY, &dev->hash_flags);
	}

	return ret;
}

static irqreturn_t s5p_aes_interrupt(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct s5p_aes_dev *dev = platform_get_drvdata(pdev);
	int err_dma_tx = 0;
	int err_dma_rx = 0;
	int err_dma_hx = 0;
	bool tx_end = false;
	bool hx_end = false;
	unsigned long flags;
	u32 status, st_bits;
	int err;

	FLOW_LOG("s5p_sss: irq\n");

	spin_lock_irqsave(&dev->lock, flags);
	/*
	 * Handle rx or tx interrupt. If there is still data (scatterlist did not
	 * reach end), then map next scatterlist entry.
	 * In case of such mapping error, s5p_aes_complete() should be called.
	 *
	 * If there is no more data in tx scatter list, call s5p_aes_complete()
	 * and schedule new tasklet.
	 *
	 * Handle hx interrupt. If there is still data map next entry.
	 */
	status = SSS_READ(dev, FCINTSTAT);
	if (status & SSS_FCINTSTAT_BRDMAINT)
		err_dma_rx = s5p_aes_rx(dev);

	if (status & SSS_FCINTSTAT_BTDMAINT) {
		if (sg_is_last(dev->sg_dst))
			tx_end = true;
		err_dma_tx = s5p_aes_tx(dev);
	}

	if (status & SSS_FCINTSTAT_HRDMAINT)
		err_dma_hx = s5p_hash_rx(dev);

	st_bits = status & (SSS_FCINTSTAT_BRDMAINT | SSS_FCINTSTAT_BTDMAINT |
				SSS_FCINTSTAT_HRDMAINT);
	/* clear DMA bits */
	SSS_WRITE(dev, FCINTPEND, st_bits);

	/* clear HASH irq bits */
	if (status & (SSS_FCINTSTAT_HDONEINT | SSS_FCINTSTAT_HPARTINT)) {
		/* cannot have both HPART and HDONE */
		if (status & SSS_FCINTSTAT_HPARTINT) {
			FLOW_LOG("s5p_sss: irq HPART\n");
			st_bits = SSS_HASH_STATUS_PARTIAL_DONE;
		}

		if (status & SSS_FCINTSTAT_HDONEINT) {
			FLOW_LOG("s5p_sss: irq HDONE\n");
			st_bits = SSS_HASH_STATUS_MSG_DONE;
		}

		set_bit(HASH_FLAGS_OUTPUT_READY, &dev->hash_flags);
		s5p_hash_write(dev, SSS_REG_HASH_STATUS, st_bits);
		hx_end = true;
		/* when DONE or PART, do not handle HASH DMA */
		err_dma_hx = 0;
	}

	if (err_dma_rx < 0) {
		err = err_dma_rx;
		goto error;
	}
	if (err_dma_tx < 0) {
		err = err_dma_tx;
		goto error;
	}

	FLOW_LOG("s5p_sss: hx_end=%d err_dma_hx=%d\n", hx_end, err_dma_hx);
	if (tx_end) {
		s5p_sg_done(dev);

		if (err_dma_hx == 1)
			s5p_set_dma_hashdata(dev, dev->hash_sg_iter);

		spin_unlock_irqrestore(&dev->lock, flags);

		s5p_aes_complete(dev, 0);
		/* Device is still busy */
		tasklet_schedule(&dev->tasklet);
	} else {
		/*
		 * Writing length of DMA block (either receiving or
		 * transmitting) will start the operation immediately, so this
		 * should be done at the end (even after clearing pending
		 * interrupts to not miss the interrupt).
		 */
		if (err_dma_tx == 1)
			s5p_set_dma_outdata(dev, dev->sg_dst);
		if (err_dma_rx == 1)
			s5p_set_dma_indata(dev, dev->sg_src);
		if (err_dma_hx == 1)
			s5p_set_dma_hashdata(dev, dev->hash_sg_iter);

		spin_unlock_irqrestore(&dev->lock, flags);
	}

	goto hash_irq_end;

error:
	s5p_sg_done(dev);
	dev->busy = false;
	if (err_dma_hx == 1)
		s5p_set_dma_hashdata(dev, dev->hash_sg_iter);

	spin_unlock_irqrestore(&dev->lock, flags);
	s5p_aes_complete(dev, err);

hash_irq_end:
	/*
	 * Note about else if:
	 *   when hash_sg_iter reaches end and its UPDATE op,
	 *   issue SSS_HASH_PAUSE and wait for HPART irq
	 */
	if (hx_end)
		tasklet_schedule(&dev->hash_tasklet);
	else if ((err_dma_hx == 2) &&
		!test_bit(HASH_FLAGS_FINAL, &dev->hash_flags))
		s5p_hash_write(dev, SSS_REG_HASH_CTRL_PAUSE,
			       SSS_HASH_PAUSE);

	return IRQ_HANDLED;
}

/**
 * s5p_hash_wait - wait for HASH status bit
 * @dd:		secss device
 * @offset:	offset for HASH register
 * @bit:	status bit
 */
static inline int s5p_hash_wait(struct s5p_aes_dev *dd, u32 offset, u32 bit)
{
	unsigned long timeout = jiffies + DEFAULT_TIMEOUT_INTERVAL;

	FLOW_LOG(__func__);
	while (!(s5p_hash_read(dd, offset) & bit)) {
		if (time_is_before_jiffies(timeout))
			return -ETIMEDOUT;
	}

	return 0;
}

/**
 * s5p_hash_read_msg - read message or IV from HW
 * @req:	AHASH request
 */
static void s5p_hash_read_msg(struct ahash_request *req)
{
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(req);
	struct s5p_aes_dev *dd = ctx->dd;
	u32 *hash = (u32 *)ctx->digest;
	int i;

	FLOW_LOG(__func__);
	for (i = 0; i < ctx->nregs; i++)
		hash[i] = s5p_hash_read(dd, SSS_REG_HASH_OUT(i));
}

/**
 * s5p_hash_write_ctx_iv - write IV for next partial/finup op.
 * @dd:		device
 * @ctx:	request context
 */
static void s5p_hash_write_ctx_iv(struct s5p_aes_dev *dd,
				  struct s5p_hash_reqctx *ctx)
{
	u32 *hash = (u32 *)ctx->digest;
	int i;

	FLOW_LOG(__func__);
	for (i = 0; i < ctx->nregs; i++)
		s5p_hash_write(dd, SSS_REG_HASH_IV(i), hash[i]);
}

/**
 * s5p_hash_write_iv - write IV for next partial/finup op.
 * @req:	AHASH request
 */
static void s5p_hash_write_iv(struct ahash_request *req)
{
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(req);
	struct s5p_aes_dev *dd = ctx->dd;

	s5p_hash_write_ctx_iv(dd, ctx);
}

/**
 * s5p_hash_copy_result - copy digest into req->result
 * @req:	AHASH request
 */
static void s5p_hash_copy_result(struct ahash_request *req)
{
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(req);
	int d = ctx->nregs;

	FLOW_LOG(__func__);
	if (!req->result)
		return;

	FLOW_DUMP("digest msg: ", ctx->digest, d * HASH_REG_SIZEOF);
	memcpy(req->result, (u8 *)ctx->digest, d * HASH_REG_SIZEOF);
}

/**
 * s5p_hash_dma_flush - flush HASH DMA
 * @dev:	secss device
 */
static void s5p_hash_dma_flush(struct s5p_aes_dev *dev)
{
	FLOW_LOG("s5p_sss: %s\n", __func__);
	SSS_WRITE(dev, FCHRDMAC, SSS_FCHRDMAC_FLUSH);
}

/**
 * s5p_hash_dma_enable()
 * @dev:	secss device
 *
 * enable DMA mode for HASH
 */
static void s5p_hash_dma_enable(struct s5p_aes_dev *dev)
{
	FLOW_LOG("s5p_sss: %s\n", __func__);
	s5p_hash_write(dev, SSS_REG_HASH_CTRL_FIFO, SSS_HASH_FIFO_MODE_DMA);
}

/**
 * s5p_hash_irq_disable - disable irq HASH signals
 * @dev:	secss device
 * @flags:	bitfield with irq's to be disabled
 *
 * SSS_FCINTENCLR_HRDMAINTENCLR
 * SSS_FCINTENCLR_HDONEINTENCLR
 * SSS_FCINTENCLR_HPARTINTENCLR
 */
static void s5p_hash_irq_disable(struct s5p_aes_dev *dev, u32 flags)
{
	FLOW_LOG("s5p_sss: %s\n", __func__);
	SSS_WRITE(dev, FCINTENCLR, flags);
}

/**
 * s5p_hash_irq_enable - enable irq signals
 * @dev:	secss device
 * @flags:	bitfield with irq's to be enabled
 *
 * SSS_FCINTENSET_HRDMAINTENSET
 * SSS_FCINTENSET_HDONEINTENSET
 * SSS_FCINTENSET_HPARTINTENSET
 */
static void s5p_hash_irq_enable(struct s5p_aes_dev *dev, int flags)
{
	FLOW_LOG("s5p_sss: %s\n", __func__);
	SSS_WRITE(dev, FCINTENSET, flags);
}

/**
 * s5p_hash_set_flow()
 * @dev:	secss device
 * @hashflow:	HASH stream flow with/without crypto AES/DES
 *
 */
static void s5p_hash_set_flow(struct s5p_aes_dev *dev, u32 hashflow)
{
	unsigned long flags;
	u32 flow;

	FLOW_LOG("s5p_sss: %s\n", __func__);
	spin_lock_irqsave(&dev->lock, flags);

	flow = SSS_READ(dev, FCFIFOCTRL);

	hashflow &= SSS_HASHIN_MASK;
	flow &= ~SSS_HASHIN_MASK;
	flow |= hashflow;

	SSS_WRITE(dev, FCFIFOCTRL, hashflow);

	spin_unlock_irqrestore(&dev->lock, flags);
}

/**
 * s5p_ahash_dma_init -
 * @dev:	secss device
 * @hashflow:	HASH stream flow with/without AES/DES
 *
 * flush HASH DMA and enable DMA,
 * set HASH stream flow inside SecSS HW
 * enable HASH irq's HRDMA, HDONE, HPART
 */
static void s5p_ahash_dma_init(struct s5p_aes_dev *dev, u32 hashflow)
{
	FLOW_LOG("s5p_sss: %s\n", __func__);
	s5p_hash_irq_disable(dev, SSS_FCINTENCLR_HRDMAINTENCLR |
			     SSS_FCINTENCLR_HDONEINTENCLR |
			     SSS_FCINTENCLR_HPARTINTENCLR);
	s5p_hash_dma_flush(dev);

/*	SSS_WRITE(dev, FCHRDMAC, SSS_FCHRDMAC_BYTESWAP); swap on */

	s5p_hash_dma_enable(dev);
	s5p_hash_set_flow(dev, hashflow);

	s5p_hash_irq_enable(dev, SSS_FCINTENSET_HRDMAINTENSET |
			    SSS_FCINTENSET_HDONEINTENSET |
			    SSS_FCINTENSET_HPARTINTENSET);
}

/**
 * s5p_hash_hw_init -
 * @dev:	secss device
 */
static int s5p_hash_hw_init(struct s5p_aes_dev *dev)
{
	set_bit(HASH_FLAGS_INIT, &dev->hash_flags);
	s5p_ahash_dma_init(dev, SSS_HASHIN_INDEPENDENT);

	return 0;
}

/**
 * s5p_hash_write_ctrl -
 * @dd:		secss device
 * @length:	length for request
 * @final:	0=not final
 *
 * Prepare SSS HASH block for processing bytes in DMA mode.
 * If it is called after previous updates, fill up IV words.
 * For final, calculate and set lengths for SSS HASH so it can
 * finalize hash.
 * For partial, set SSS HASH length as 2^63 so it will be never
 * reached and set to zero prelow and prehigh.
 *
 * This function do not start DMA transfer.
 */
static void s5p_hash_write_ctrl(struct s5p_aes_dev *dd, size_t length,
				int final)
{
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(dd->hash_req);
	u32 configflags, swapflags;
	u32 prelow, prehigh, low, high;
	u64 tmplen;

	FLOW_LOG("s5p_sss: %s engine: 0x%x digcnt=%lld\n", __func__,
		 ctx->engine, ctx->digcnt);
	configflags = ctx->engine | SSS_HASH_INIT_BIT;

	if (likely(ctx->digcnt)) {
		s5p_hash_write_ctx_iv(dd, ctx);
		configflags |= SSS_HASH_USER_IV_EN;
	}

	if (final) {
		/* number of bytes for last part */
		low = length; high = 0;
		/* total number of bits prev hashed */
		tmplen = ctx->digcnt * 8;
		prelow = (u32)tmplen;
		prehigh = (u32)(tmplen >> 32);
		FLOW_LOG("s5p_sss: %s final, length=%d tmplen=%llx\n", __func__,
			low, tmplen);
	} else {
		FLOW_LOG("s5p_sss: %s partial\n", __func__);
		prelow = 0; prehigh = 0;
		low = 0; high = BIT(31);
	}

	swapflags = SSS_HASH_BYTESWAP_DI | SSS_HASH_BYTESWAP_DO |
		    SSS_HASH_BYTESWAP_IV | SSS_HASH_BYTESWAP_KEY;

	s5p_hash_write(dd, SSS_REG_HASH_MSG_SIZE_LOW, low);
	s5p_hash_write(dd, SSS_REG_HASH_MSG_SIZE_HIGH, high);
	s5p_hash_write(dd, SSS_REG_HASH_PRE_MSG_SIZE_LOW, prelow);
	s5p_hash_write(dd, SSS_REG_HASH_PRE_MSG_SIZE_HIGH, prehigh);

	s5p_hash_write(dd, SSS_REG_HASH_CTRL_SWAP, swapflags);
	s5p_hash_write(dd, SSS_REG_HASH_CTRL, configflags);
}

/**
 * s5p_hash_xmit_dma - start DMA hash processing
 * @dd:		secss device
 * @length:	length for request
 * @final:	0=not final
 *
 * Map ctx->sg into DMA_TO_DEVICE,
 * remember sg and cnt in device dd->hash_sg_iter, dd->hash_sg_cnt
 * so it can be used in loop inside irq handler.
 * Update ctx->digcnt, need this to keep number of processed bytes
 * for last final/finup request.
 * Set dma address and length, this starts DMA,
 * return with -EINPROGRESS.
 * HW HASH block will issue signal for irq handler.
 */
static int s5p_hash_xmit_dma(struct s5p_aes_dev *dd, size_t length,
			      int final)
{
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(dd->hash_req);
	int cnt;

	dev_dbg(dd->dev, "xmit_dma: digcnt: %lld, length: %d, final: %d\n",
						ctx->digcnt, length, final);

	cnt = dma_map_sg(dd->dev, ctx->sg, ctx->sg_len, DMA_TO_DEVICE);
	if (!cnt) {
		dev_err(dd->dev, "dma_map_sg error\n");
		set_bit(HASH_FLAGS_ERROR, &ctx->flags);
		return -EINVAL;
	}

	FLOW_LOG("xmit_dma");
	set_bit(HASH_FLAGS_DMA_ACTIVE, &dd->hash_flags);

	dd->hash_sg_iter = ctx->sg;
	dd->hash_sg_cnt = cnt;
	FLOW_LOG("xmit_dma cnt=%d final=%d len=%d", cnt, final, length);

	s5p_hash_write_ctrl(dd, length, final);

	/* update digcnt in request */
	ctx->digcnt += length;
	ctx->total -= length;

	/* catch last interrupt */
	if (final)
		set_bit(HASH_FLAGS_FINAL, &dd->hash_flags);

	s5p_set_dma_hashdata(dd, dd->hash_sg_iter); /* DMA starts */

	return -EINPROGRESS;
}

/**
 * s5p_hash_copy_sgs -
 * @ctx:	request context
 * @sg:		source scatterlist request
 * @bs:		block size
 * @new_len:	number of bytes to process from sg
 *
 * Allocate new buffer, copy data for HASH into it.
 * If there was xmit_buf filled, copy it first, then
 * copy data from sg into it.
 * Prepare one sgl[0] with allocated buffer.
 *
 * Set ctx->sg to sgl[0].
 * Set flag so we can free it after irq ends processing.
 */
static int s5p_hash_copy_sgs(struct s5p_hash_reqctx *ctx,
			     struct scatterlist *sg, int bs, int new_len)
{
	int pages;
	void *buf;
	int len;

	FLOW_LOG("copy_sgs new_len=%d", new_len);
	len = new_len + ctx->bufcnt;

	FLOW_LOG("copy_sgs len=%d", len);
	pages = get_order(len); /* ctx->total); */

	buf = (void *)__get_free_pages(GFP_ATOMIC, pages);
	if (!buf) {
		dev_err(ctx->dd->dev, "alloc pages for unaligned case.\n");
		set_bit(HASH_FLAGS_ERROR, &ctx->flags);
		return -ENOMEM;
	}

	if (ctx->bufcnt)
		memcpy(buf, ctx->dd->xmit_buf, ctx->bufcnt);

	scatterwalk_map_and_copy(buf + ctx->bufcnt, sg, ctx->skip,
				 new_len, 0);
	sg_init_table(ctx->sgl, 1);
	sg_set_buf(ctx->sgl, buf, len);
	ctx->sg = ctx->sgl;
	ctx->sg_len = 1;
	ctx->bufcnt = 0;
	ctx->skip = 0;
	set_bit(HASH_FLAGS_SGS_COPIED, &ctx->dd->hash_flags);

	return 0;
}

/**
 * s5p_hash_copy_sg_lists -
 * @rctx:	request context
 * @sg:		source scatterlist request
 * @bs:		block size
 * @new_len:	number of bytes to process from sg
 *
 * Allocate new scatterlist table, copy data for HASH into it.
 * If there was xmit_buf filled, prepare it first, then
 * copy page, length and offset from source sg into it,
 * adjusting begin and/or end for skip offset and hash_later value.
 *
 * Resulting sg table will be assigned to ctx->sg.
 * Set flag so we can free it after irq ends processing.
 */
static int s5p_hash_copy_sg_lists(struct s5p_hash_reqctx *ctx,
				  struct scatterlist *sg, int bs, int new_len)
{
	int n = sg_nents(sg);
	struct scatterlist *tmp;
	int offset = ctx->skip;

	FLOW_LOG("copy_sg_lists n=%d", n);
	if (ctx->bufcnt)
		n++;

	FLOW_LOG("copy_sg_lists n=%d, alloc struct sg", n);
	ctx->sg = kmalloc_array(n, sizeof(*sg), GFP_KERNEL);
	if (!ctx->sg) {
		dev_err(ctx->dd->dev, "alloc sg for unaligned case.\n");
		set_bit(HASH_FLAGS_ERROR, &ctx->flags);
		return -ENOMEM;
	}

	sg_init_table(ctx->sg, n);

	tmp = ctx->sg;

	ctx->sg_len = 0;

	if (ctx->bufcnt) {
		sg_set_buf(tmp, ctx->dd->xmit_buf, ctx->bufcnt);
		tmp = sg_next(tmp);
		ctx->sg_len++;
	}

	while (sg && new_len) {
		int len = sg->length - offset;

		if (offset) {
			offset -= sg->length;
			if (offset < 0)
				offset = 0;
		}

		if (new_len < len)
			len = new_len;

		if (len > 0) {
			new_len -= len;
			sg_set_page(tmp, sg_page(sg), len, sg->offset);
			if (new_len <= 0)
				sg_mark_end(tmp);
			tmp = sg_next(tmp);
			ctx->sg_len++;
		}

		sg = sg_next(sg);
	}

	set_bit(HASH_FLAGS_SGS_ALLOCED, &ctx->dd->hash_flags);

	ctx->bufcnt = 0;

	return 0;
}

/**
 * s5p_hash_prepare_sgs -
 * @sg:		source scatterlist request
 * @nbytes:	number of bytes to process from sg
 * @bs:		block size
 * @final:	final flag
 * @rctx:	request context
 *
 * Check two conditions: (1) if buffers in sg have len aligned data,
 * and (2) sg table have good aligned elements (list_ok)
 * If one of this checks fails, then either
 * (1) allocates new buffer for data with s5p_hash_copy_sgs,
 * copy data into this buffer and prepare request in sgl, or
 * (2) allocates new sg table and prepare sg elements
 *
 * For digest or finup all conditions can be good, and we may not need
 * any fixes.
 */
static int s5p_hash_prepare_sgs(struct scatterlist *sg,
				int nbytes, int bs, bool final,
				struct s5p_hash_reqctx *rctx)
{
	int n = 0;
	bool aligned = true;
	bool list_ok = true;
	struct scatterlist *sg_tmp = sg;
	int offset = rctx->skip;
	int new_len;

	FLOW_LOG("prepare_sgs nbytes=%d bs=%d, final=%d", nbytes, bs, final);
	if (!sg || !sg->length || !nbytes)
		return 0;

	new_len = nbytes;

	if (offset)
		list_ok = false;

	if (!final)
		list_ok = false;

	while (nbytes > 0 && sg_tmp) {
		n++;

		if (offset < sg_tmp->length) {
#if 0
			if (!IS_ALIGNED(offset + sg_tmp->offset, 4)) {
				aligned = false;
				break;
			}
#endif
			if (!IS_ALIGNED(sg_tmp->length - offset, bs)) {
				aligned = false;
				break;
			}
		}

		if (!sg_tmp->length) {
			aligned = false;
			break;
		}

		if (offset) {
			offset -= sg_tmp->length;
			if (offset < 0) {
				nbytes += offset;
				offset = 0;
			}
		} else {
			nbytes -= sg_tmp->length;
		}

		sg_tmp = sg_next(sg_tmp);

		if (nbytes < 0) { /* when hash_later is > 0 */
			list_ok = false;
			break;
		}
	}

	if (!aligned)
		return s5p_hash_copy_sgs(rctx, sg, bs, new_len);
	else if (!list_ok)
		return s5p_hash_copy_sg_lists(rctx, sg, bs, new_len);

	/* have aligned data from previous operation and/or current
	 * Note: will enter here only if (digest or finup) and aligned
	 */
	if (rctx->bufcnt) {
		FLOW_LOG("prepare_sgs xmit_buf chained with sg sg_len=%d", n+1);
		rctx->sg_len = n;
		sg_init_table(rctx->sgl, 2);
		sg_set_buf(rctx->sgl, rctx->dd->xmit_buf, rctx->bufcnt);
		sg_chain(rctx->sgl, 2, sg);
		rctx->sg = rctx->sgl;
		rctx->sg_len++;
	} else {
		FLOW_LOG("prepare_sgs no xmit_buf, original sg sg_len=%d", n);
		rctx->sg = sg;
		rctx->sg_len = n;
	}

	return 0;
}

/**
 * s5p_hash_prepare_request -
 * @req:	AHASH request
 * @update:	true if UPDATE op
 *
 * Note 1: we can have update flag _and_ final flag at the same time.
 * Note 2: we enter here when digcnt > BUFLEN (=HASH_BLOCK_SIZE) or
 *	   either req->nbytes or ctx->bufcnt + req->nbytes is > BUFLEN or
 *	   we have final op
 */
static int s5p_hash_prepare_request(struct ahash_request *req, bool update)
{
	struct s5p_hash_reqctx *rctx = ahash_request_ctx(req);
	int bs;
	int ret;
	int nbytes;
	bool final = rctx->flags & BIT(HASH_FLAGS_FINUP);
	int xmit_len, hash_later;

	FLOW_LOG("prepare_req update=%d final=%d", update, final);
	if (!req)
		return 0;

	bs = BUFLEN;

	if (update)
		nbytes = req->nbytes;
	else
		nbytes = 0;

	rctx->total = nbytes + rctx->bufcnt;

	FLOW_LOG("prepare_req total=%d", rctx->total);
	if (!rctx->total)
		return 0;

	FLOW_LOG("prepare_req nbytes=%d bufcnt=%d", nbytes, rctx->bufcnt);
	if (nbytes && (!IS_ALIGNED(rctx->bufcnt, BUFLEN))) {
		/* bytes left from previous request, so fill up to BUFLEN */
		int len = BUFLEN - rctx->bufcnt % BUFLEN;

		FLOW_LOG("prepare_req fillup buffer, needed len=%d", len);
		if (len > nbytes)
			len = nbytes;
		FLOW_LOG("prepare_req fillup, len=%d", len);
		scatterwalk_map_and_copy(rctx->buffer + rctx->bufcnt, req->src,
					 0, len, 0);
		rctx->bufcnt += len;
		nbytes -= len;
		rctx->skip = len;
		FLOW_LOG("prepare_req nbytes=%d bufcnt=%d skip=%d",
			 nbytes, rctx->bufcnt, rctx->skip);
	} else {
		rctx->skip = 0;
		FLOW_LOG("prepare_req skip=%d", rctx->skip);
	}

	if (rctx->bufcnt)
		memcpy(rctx->dd->xmit_buf, rctx->buffer, rctx->bufcnt);

	xmit_len = rctx->total;
	if (final) {
		hash_later = 0;
		FLOW_LOG("prepare_req final, zero hash_later");
	} else {
		if (IS_ALIGNED(xmit_len, bs))
			xmit_len -= bs;
		else
			xmit_len -= xmit_len & (bs - 1);

		hash_later = rctx->total - xmit_len;
		WARN_ON(req->nbytes == 0);
		WARN_ON(hash_later <= 0);
		/* == if bufcnt was BUFLEN */
		WARN_ON(req->nbytes < hash_later);
		WARN_ON(rctx->skip > (req->nbytes - hash_later));
		/* copy hash_later bytes from end of req->src */
		/* previous bytes are in xmit_buf, so no overwrite */
		FLOW_LOG("prepare_req copy tail to buffer, off=%d, count=%d",
			 req->nbytes - hash_later, hash_later);
		scatterwalk_map_and_copy(rctx->buffer, req->src,
					 req->nbytes - hash_later,
					 hash_later, 0);
	}

	WARN_ON(hash_later < 0);
	WARN_ON(nbytes < hash_later);

	if (xmit_len > bs) {
		FLOW_LOG("prepare_req xmit_len > bs %d %d", xmit_len, bs);
		WARN_ON(nbytes <= hash_later);
		ret = s5p_hash_prepare_sgs(req->src, nbytes - hash_later, bs,
					   final, rctx);
		if (ret)
			return ret;
	} else {
		/* have buffered data only */
		FLOW_LOG("prepare_req data xmit_len=%d, bufcnt=%d",
			 xmit_len, rctx->bufcnt);
		if (unlikely(!rctx->bufcnt)) {
			/* first update didn't fill up buffer */
			WARN_ON(xmit_len != BUFLEN);
			scatterwalk_map_and_copy(rctx->dd->xmit_buf, req->src,
				0, xmit_len, 0);
		}

		sg_init_table(rctx->sgl, 1);
		sg_set_buf(rctx->sgl, rctx->dd->xmit_buf, xmit_len);

		rctx->sg = rctx->sgl;
		rctx->sg_len = 1;
	}

	FLOW_LOG("prepare_req hash_later=%d", hash_later);
	rctx->bufcnt = hash_later;
	if (!final)
		rctx->total = xmit_len;

	return 0;
}

/**
 * s5p_hash_update_dma_stop()
 * @dd:		secss device
 *
 * Unmap scatterlist ctx->sg.
 */
static int s5p_hash_update_dma_stop(struct s5p_aes_dev *dd)
{
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(dd->hash_req);

	dma_unmap_sg(dd->dev, ctx->sg, ctx->sg_len, DMA_TO_DEVICE);

	clear_bit(HASH_FLAGS_DMA_ACTIVE, &dd->hash_flags);

	return 0;
}

/**
 * s5p_hash_update_req - process AHASH request
 * @dd:		device s5p_aes_dev
 *
 * Processes the input data from AHASH request using DMA
 *
 * Current request should have ctx->sg prepared before.
 *
 * Returns: see s5p_hash_final below.
 */
static int s5p_hash_update_req(struct s5p_aes_dev *dd)
{
	struct ahash_request *req = dd->hash_req;
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(req);
	int err;
	bool final = ctx->flags & BIT(HASH_FLAGS_FINUP);

	dev_dbg(dd->dev, "update_req: total: %u, digcnt: %lld, finup: %d\n",
		 ctx->total, ctx->digcnt, final);

	err = s5p_hash_xmit_dma(dd, ctx->total, final);

	/* wait for dma completion before can take more data */
	dev_dbg(dd->dev, "update: err: %d, digcnt: %lld\n", err, ctx->digcnt);

	return err;
}

/**
 * s5p_hash_final_req - process the final AHASH request
 * @dd:		device s5p_aes_dev
 *
 * Processes the input data from the last AHASH request
 * using . Resets the buffer counter (ctx->bufcnt)
 *
 * Returns: see s5p_hash_final below.
 */
static int s5p_hash_final_req(struct s5p_aes_dev *dd)
{
	struct ahash_request *req = dd->hash_req;
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(req);
	int err = 0;

	err = s5p_hash_xmit_dma(dd, ctx->total, 1);
	ctx->bufcnt = 0;
	dev_dbg(dd->dev, "final_req: err: %d\n", err);

	return err;
}

/**
 * s5p_hash_finish - copy calculated digest to crypto layer
 * @req:	AHASH request
 *
 * Copies the calculated hash value to the buffer provided
 * by req->result
 *
 * Returns 0 on success and negative values on error.
 */
static int s5p_hash_finish(struct ahash_request *req)
{
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(req);
	struct s5p_aes_dev *dd = ctx->dd;
	int err = 0;

	if (ctx->digcnt)
		s5p_hash_copy_result(req);

	dev_dbg(dd->dev, "digcnt: %lld, bufcnt: %d\n", ctx->digcnt,
		ctx->bufcnt);

	return err;
}

/**
 * s5p_hash_finish_req - finish request
 * @req:	AHASH request
 * @err:	error
 *
 * Clear flags, free memory,
 * if FINAL then read output into ctx->digest,
 * call completetion
 */
static void s5p_hash_finish_req(struct ahash_request *req, int err)
{
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(req);
	struct s5p_aes_dev *dd = ctx->dd;

	FLOW_LOG("s5p_sss: hash_finish_req\n");

	if (test_bit(HASH_FLAGS_SGS_COPIED, &dd->hash_flags))
		free_pages((unsigned long)sg_virt(ctx->sg),
			   get_order(ctx->sg->length));

	if (test_bit(HASH_FLAGS_SGS_ALLOCED, &dd->hash_flags))
		kfree(ctx->sg);

	ctx->sg = NULL;

	dd->hash_flags &= ~(BIT(HASH_FLAGS_SGS_ALLOCED) |
			    BIT(HASH_FLAGS_SGS_COPIED));

	if (!err && !test_bit(HASH_FLAGS_ERROR, &ctx->flags)) {
		FLOW_LOG("s5p_sss: hash__finish_req read msg\n");
		s5p_hash_read_msg(req);
		if (test_bit(HASH_FLAGS_FINAL, &dd->hash_flags))
			err = s5p_hash_finish(req);
	} else {
		FLOW_LOG("s5p_sss: hash__finish_req error, no read msg\n");
		ctx->flags |= BIT(HASH_FLAGS_ERROR);
	}

	/* atomic operation is not needed here */
	dd->hash_flags &= ~(BIT(HASH_FLAGS_BUSY) | BIT(HASH_FLAGS_FINAL) |
			    BIT(HASH_FLAGS_DMA_READY) |
			    BIT(HASH_FLAGS_OUTPUT_READY));

	if (req->base.complete)
		req->base.complete(&req->base, err);
}

/**
 * s5p_hash_handle_queue - handle hash queue
 * @dd:		device s5p_aes_dev
 * @req:	AHASH request
 *
 * If req!=NULL enqueue it
 *
 * Enqueues the current AHASH request on dd->queue and
 * if FLAGS_BUSY is not set on the device then processes
 * the first request from the dd->queue
 *
 * Returns: see s5p_hash_final below.
 */
static int s5p_hash_handle_queue(struct s5p_aes_dev *dd,
				  struct ahash_request *req)
{
	struct crypto_async_request *async_req, *backlog;
	struct s5p_hash_reqctx *ctx;
	unsigned long flags;
	int err = 0, ret = 0;

retry:
	FLOW_LOG("s5p_sss: hash_handle_queue\n");
	spin_lock_irqsave(&dd->hash_lock, flags);
	if (req)
		ret = ahash_enqueue_request(&dd->hash_queue, req);
	if (test_bit(HASH_FLAGS_BUSY, &dd->hash_flags)) {
		spin_unlock_irqrestore(&dd->hash_lock, flags);
		FLOW_LOG("s5p_sss: hash_handle_queue - exit, busy\n");
		return ret;
	}
	backlog = crypto_get_backlog(&dd->hash_queue);
	async_req = crypto_dequeue_request(&dd->hash_queue);
	if (async_req)
		set_bit(HASH_FLAGS_BUSY, &dd->hash_flags);
	spin_unlock_irqrestore(&dd->hash_lock, flags);

	if (!async_req) {
		FLOW_LOG("s5p_sss: hash_handle_queue - exit, empty\n");
		return ret;
	}

	FLOW_LOG("s5p_sss: hash_handle_queue - backlog\n");
	if (backlog)
		backlog->complete(backlog, -EINPROGRESS);

	FLOW_LOG("s5p_sss: hash_handle_queue - async_req\n");
	req = ahash_request_cast(async_req);
	dd->hash_req = req;
	ctx = ahash_request_ctx(req);

	FLOW_LOG("s5p_sss: hash_handle_queue - prepare_req\n");
	err = s5p_hash_prepare_request(req, ctx->op == HASH_OP_UPDATE);
	if (err || !ctx->total)
		goto err1;

	dev_dbg(dd->dev, "handling new req, op: %u, nbytes: %d\n",
						ctx->op, req->nbytes);

	err = s5p_hash_hw_init(dd);
	if (err)
		goto err1;

	dd->hash_err = 0;
	if (ctx->digcnt)
		/* request has changed - restore hash */
		s5p_hash_write_iv(req);

	if (ctx->op == HASH_OP_UPDATE) {
		FLOW_LOG("s5p_sss: hash_handle_queue - op=UPDATE, finup=%d\n",
			 (ctx->flags & BIT(HASH_FLAGS_FINUP)) != 0);
		err = s5p_hash_update_req(dd);
		if (err != -EINPROGRESS &&
		   (ctx->flags & BIT(HASH_FLAGS_FINUP)))
			/* no final() after finup() */
			err = s5p_hash_final_req(dd);
	} else if (ctx->op == HASH_OP_FINAL) {
		FLOW_LOG("s5p_sss: hash_handle_queue - op=FINAL\n");
		err = s5p_hash_final_req(dd);
	}
err1:
	dev_dbg(dd->dev, "exit, err: %d\n", err);

	if (err != -EINPROGRESS) {
		/* hash_tasklet_cb will not finish it, so do it here */
		s5p_hash_finish_req(req, err);
		req = NULL;

		/*
		 * Execute next request immediately if there is anything
		 * in queue.
		 */
		FLOW_LOG("s5p_sss: hash_handle_queue - retry\n");
		goto retry;
	}

	FLOW_LOG("s5p_sss: hash_handle_queue - exit, ret=%d\n", ret);

	return ret;
}

/**
 * s5p_hash_tasklet_cb - hash tasklet
 * @data:	ptr to s5p_aes_dev
 *
 */
static void s5p_hash_tasklet_cb(unsigned long data)
{
	struct s5p_aes_dev *dd = (struct s5p_aes_dev *)data;
	int err = 0;

	FLOW_LOG("s5p_sss: hash_tasklet\n");
	if (!test_bit(HASH_FLAGS_BUSY, &dd->hash_flags)) {
		FLOW_LOG("s5p_sss: hash_tasklet not BUSY, handle queue\n");
		s5p_hash_handle_queue(dd, NULL);
		return;
	}

	if (test_bit(HASH_FLAGS_DMA_READY, &dd->hash_flags)) {
		FLOW_LOG("s5p_sss: hash_tasklet DMA_READY\n");
		if (test_and_clear_bit(HASH_FLAGS_DMA_ACTIVE,
				       &dd->hash_flags)) {
			FLOW_LOG("s5p_sss: hash_tasklet DMA_ACTIVE cleared\n");
			s5p_hash_update_dma_stop(dd);
			if (dd->hash_err) {
				FLOW_LOG("s5p_sss: hash_tasklet hash_error\n");
				err = dd->hash_err;
				goto finish;
			}
		}
		if (test_and_clear_bit(HASH_FLAGS_OUTPUT_READY,
				       &dd->hash_flags)) {
			/* hash or semi-hash ready */
			FLOW_LOG("s5p_sss: hash_tasklet OUTPUT_READY\n");
			clear_bit(HASH_FLAGS_DMA_READY, &dd->hash_flags);
				goto finish;
		}
	}

	return;

finish:
	FLOW_LOG("s5p_sss: hash_tasklet finish\n");
	dev_dbg(dd->dev, "update done: err: %d\n", err);
	/* finish curent request */
	s5p_hash_finish_req(dd->hash_req, err);

	/* If we are not busy, process next req */
	if (!test_bit(HASH_FLAGS_BUSY, &dd->hash_flags))
		s5p_hash_handle_queue(dd, NULL);
}

/**
 * s5p_hash_enqueue - enqueue request
 * @req:	AHASH request
 * @op:		operation UPDATE or FINAL
 *
 * Sets the operation flag in the AHASH request context
 * structure and calls s5p_hash_handle_queue().
 *
 * Returns: see s5p_hash_final below.
 */
static int s5p_hash_enqueue(struct ahash_request *req, unsigned int op)
{
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(req);
	struct s5p_hash_ctx *tctx = crypto_tfm_ctx(req->base.tfm);
	struct s5p_aes_dev *dd = tctx->dd;

	ctx->op = op;

	return s5p_hash_handle_queue(dd, req);
}

/**
 * s5p_hash_update - process the hash input data
 * @req:	AHASH request
 *
 * If request will fit in buffer, copy it and return immediately
 * else enqueue it wit OP_UPDATE.
 *
 * Returns: see s5p_hash_final below.
 */
static int s5p_hash_update(struct ahash_request *req)
{
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(req);

	FLOW_LOG("hash update len=%d", req->nbytes);
	if (!req->nbytes)
		return 0;

	FLOW_DUMP("upd: ", req->src, req->nbytes);
	if (ctx->bufcnt + req->nbytes <= BUFLEN) {
		scatterwalk_map_and_copy(ctx->buffer + ctx->bufcnt, req->src,
					 0, req->nbytes, 0);
		ctx->bufcnt += req->nbytes;
		return 0;
	}

	return s5p_hash_enqueue(req, HASH_OP_UPDATE);
}

/**
 * s5p_hash_shash_digest - calculate shash digest
 * @tfm:	crypto transformation
 * @flags:	tfm flags
 * @data:	input data
 * @len:	length of data
 * @out:	output buffer
 */
static int s5p_hash_shash_digest(struct crypto_shash *tfm, u32 flags,
				  const u8 *data, unsigned int len, u8 *out)
{
	SHASH_DESC_ON_STACK(shash, tfm);

	shash->tfm = tfm;
	shash->flags = flags & CRYPTO_TFM_REQ_MAY_SLEEP;

	return crypto_shash_digest(shash, data, len, out);
}

/**
 * s5p_hash_final_shash - calculate shash digest
 * @req:	AHASH request
 *
 * calculate digest from ctx->buffer,
 * with data length ctx->bufcnt,
 * store digest in req->result
 */
static int s5p_hash_final_shash(struct ahash_request *req)
{
	struct s5p_hash_ctx *tctx = crypto_tfm_ctx(req->base.tfm);
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(req);

	return s5p_hash_shash_digest(tctx->fallback, req->base.flags,
				     ctx->buffer, ctx->bufcnt, req->result);
}

/**
 * s5p_hash_final - close up hash and calculate digest
 * @req:	AHASH request
 *
 * Set FLAGS_FINUP flag for the current AHASH request context.
 *
 * If there were no input data processed yet and the buffered
 * hash data is less than BUFLEN (64) then calculate the final
 * hash immediately by using SW algorithm fallback.
 *
 * Otherwise enqueues the current AHASH request with OP_FINAL
 * operation flag and finalize hash message in HW.
 * Note that if digcnt!=0 then there were previous update op,
 * so there are always some buffered bytes in ctx->buffer,
 * which means that ctx->bufcnt!=0
 *
 * Returns:
 * 0 if the request has been processed immediately,
 * -EINPROGRESS if the operation has been queued for later
 *	execution or is set to processing by HW,
 * -EBUSY if queue is full and request should be resubmitted later,
 * other negative values on error.
 *
 * Note: req->src do not have any data
 */
static int s5p_hash_final(struct ahash_request *req)
{
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(req);

	FLOW_LOG("hash final");
	ctx->flags |= BIT(HASH_FLAGS_FINUP);

	if (ctx->flags & BIT(HASH_FLAGS_ERROR))
		return -EINVAL; /* uncompleted hash is not needed */

	/*
	 * If message is small (digcnt==0) and buffersize is less
	 * than BUFLEN, we use fallback, as using DMA + HW in this
	 * case doesn't provide any benefit.
	 * This is also the case for zero-length message.
	 */
	FLOW_LOG("hash final digcnt=%lld bufcnt=%d", ctx->digcnt, ctx->bufcnt);
	if (!ctx->digcnt && ctx->bufcnt < BUFLEN)
		return s5p_hash_final_shash(req);

	WARN_ON(ctx->bufcnt == 0);

	return s5p_hash_enqueue(req, HASH_OP_FINAL);
}

/**
 * s5p_hash_finup - process last req->src and calculate digest
 * @req:	AHASH request containing the last update data
 *
 * Set FLAGS_FINUP flag in context.
 *
 * Call update(req) and exit if it was enqueued or is being processing.
 *
 * If update returns without enqueue, call final(req).
 *
 * Return values: see s5p_hash_final above.
 */
static int s5p_hash_finup(struct ahash_request *req)
{
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(req);
	int err1, err2;

	FLOW_LOG("hash finup len=%d", req->nbytes);
	ctx->flags |= BIT(HASH_FLAGS_FINUP);

	FLOW_DUMP("fin: ", req->src, req->nbytes);
	err1 = s5p_hash_update(req);
	if (err1 == -EINPROGRESS || err1 == -EBUSY)
		return err1;
	/*
	 * final() has to be always called to cleanup resources
	 * even if update() failed, except EINPROGRESS
	 * or calculate digest for small size
	 */
	err2 = s5p_hash_final(req);

	return err1 ?: err2;
}

/**
 * s5p_hash_init - initialize AHASH request contex
 * @req:	AHASH request
 *
 * Init async hash request context.
 */
static int s5p_hash_init(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct s5p_hash_ctx *tctx = crypto_ahash_ctx(tfm);
	struct s5p_hash_reqctx *ctx = ahash_request_ctx(req);
	struct s5p_aes_dev *dd = tctx->dd;

	ctx->dd = dd;
	ctx->flags = 0;

	dev_dbg(dd->dev, "init: digest size: %d\n",
		crypto_ahash_digestsize(tfm));

	switch (crypto_ahash_digestsize(tfm)) {
	case MD5_DIGEST_SIZE:
		ctx->flags |= HASH_FLAGS_MODE_MD5;
		ctx->engine = SSS_HASH_ENGINE_MD5;
		ctx->nregs = HASH_MD5_MAX_REG;
		break;
	case SHA1_DIGEST_SIZE:
		ctx->flags |= HASH_FLAGS_MODE_SHA1;
		ctx->engine = SSS_HASH_ENGINE_SHA1;
		ctx->nregs = HASH_SHA1_MAX_REG;
		break;
	case SHA256_DIGEST_SIZE:
		ctx->flags |= HASH_FLAGS_MODE_SHA256;
		ctx->engine = SSS_HASH_ENGINE_SHA256;
		ctx->nregs = HASH_SHA256_MAX_REG;
		break;
	}

	ctx->bufcnt = 0;
	ctx->digcnt = 0;
	ctx->total = 0;
	ctx->skip = 0;
	ctx->buflen = BUFLEN;

	return 0;
}

/**
 * s5p_hash_digest - calculate digest from req->src
 * @req:	AHASH request
 *
 * Return values: see s5p_hash_final above.
 */
static int s5p_hash_digest(struct ahash_request *req)
{
	FLOW_LOG("hash digest len=%d", req->nbytes);
	FLOW_DUMP("dig: ", req->src, req->nbytes);

	return s5p_hash_init(req) ?: s5p_hash_finup(req);
}

/**
 * s5p_hash_cra_init_alg - init crypto alg transformation
 * @tfm:	crypto transformation
 */
static int s5p_hash_cra_init_alg(struct crypto_tfm *tfm)
{
	struct s5p_hash_ctx *tctx = crypto_tfm_ctx(tfm);
	const char *alg_name = crypto_tfm_alg_name(tfm);

	tctx->dd = s5p_dev;
	/* Allocate a fallback and abort if it failed. */
	tctx->fallback = crypto_alloc_shash(alg_name, 0,
					    CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(tctx->fallback)) {
		pr_err("fallback alloc fails for '%s'\n", alg_name);
		return PTR_ERR(tctx->fallback);
	}

	crypto_ahash_set_reqsize(__crypto_ahash_cast(tfm),
				 sizeof(struct s5p_hash_reqctx) + BUFLEN);

	return 0;
}

/**
 * s5p_hash_cra_init - init crypto tfm
 * @tfm:	crypto transformation
 */
static int s5p_hash_cra_init(struct crypto_tfm *tfm)
{
	return s5p_hash_cra_init_alg(tfm);
}

/**
 * s5p_hash_cra_exit - exit crypto tfm
 * @tfm:	crypto transformation
 *
 * free allocated fallback
 */
static void s5p_hash_cra_exit(struct crypto_tfm *tfm)
{
	struct s5p_hash_ctx *tctx = crypto_tfm_ctx(tfm);

	crypto_free_shash(tctx->fallback);
	tctx->fallback = NULL;
}

/**
 * s5p_hash_export - export hash state
 * @req:	AHASH request
 * @out:	buffer for exported state
 */
static int s5p_hash_export(struct ahash_request *req, void *out)
{
	struct s5p_hash_reqctx *rctx = ahash_request_ctx(req);

	FLOW_LOG("hash export");
	memcpy(out, rctx, sizeof(*rctx) + rctx->bufcnt);

	return 0;
}

/**
 * s5p_hash_import - import hash state
 * @req:	AHASH request
 * @in:		buffer with state to be imported from
 */
static int s5p_hash_import(struct ahash_request *req, const void *in)
{
	struct s5p_hash_reqctx *rctx = ahash_request_ctx(req);
	const struct s5p_hash_reqctx *ctx_in = in;

	FLOW_LOG("hash import");
	WARN_ON(ctx_in->bufcnt < 0);
	WARN_ON(ctx_in->bufcnt > BUFLEN);
	memcpy(rctx, in, sizeof(*rctx) + BUFLEN);

	return 0;
}

/**
 * struct algs_sha1_md5
 */
static struct ahash_alg algs_sha1_md5[] = {
{
	.init		= s5p_hash_init,
	.update		= s5p_hash_update,
	.final		= s5p_hash_final,
	.finup		= s5p_hash_finup,
	.digest		= s5p_hash_digest,
	.halg.digestsize	= SHA1_DIGEST_SIZE,
	.halg.base	= {
		.cra_name		= "sha1",
		.cra_driver_name	= "exynos-sha1",
		.cra_priority		= 100,
		.cra_flags		= CRYPTO_ALG_TYPE_AHASH |
					  CRYPTO_ALG_KERN_DRIVER_ONLY |
					  CRYPTO_ALG_ASYNC |
					  CRYPTO_ALG_NEED_FALLBACK,
		.cra_blocksize		= HASH_BLOCK_SIZE,
		.cra_ctxsize		= sizeof(struct s5p_hash_ctx),
		.cra_alignmask		= SSS_DMA_ALIGN_MASK,
		.cra_module		= THIS_MODULE,
		.cra_init		= s5p_hash_cra_init,
		.cra_exit		= s5p_hash_cra_exit,
	}
},
{
	.init		= s5p_hash_init,
	.update		= s5p_hash_update,
	.final		= s5p_hash_final,
	.finup		= s5p_hash_finup,
	.digest		= s5p_hash_digest,
	.halg.digestsize	= MD5_DIGEST_SIZE,
	.halg.base	= {
		.cra_name		= "md5",
		.cra_driver_name	= "exynos-md5",
		.cra_priority		= 100,
		.cra_flags		= CRYPTO_ALG_TYPE_AHASH |
					  CRYPTO_ALG_KERN_DRIVER_ONLY |
					  CRYPTO_ALG_ASYNC |
					  CRYPTO_ALG_NEED_FALLBACK,
		.cra_blocksize		= HASH_BLOCK_SIZE,
		.cra_ctxsize		= sizeof(struct s5p_hash_ctx),
		.cra_alignmask		= SSS_DMA_ALIGN_MASK,
		.cra_module		= THIS_MODULE,
		.cra_init		= s5p_hash_cra_init,
		.cra_exit		= s5p_hash_cra_exit,
	}
}
};

/**
 * struct algs_sha256
 */
static struct ahash_alg algs_sha256[] = {
{
	.init		= s5p_hash_init,
	.update		= s5p_hash_update,
	.final		= s5p_hash_final,
	.finup		= s5p_hash_finup,
	.digest		= s5p_hash_digest,
	.halg.digestsize	= SHA256_DIGEST_SIZE,
	.halg.base	= {
		.cra_name		= "sha256",
		.cra_driver_name	= "exynos-sha256",
		.cra_priority		= 100,
		.cra_flags		= CRYPTO_ALG_TYPE_AHASH |
					  CRYPTO_ALG_KERN_DRIVER_ONLY |
					  CRYPTO_ALG_ASYNC |
					  CRYPTO_ALG_NEED_FALLBACK,
		.cra_blocksize		= HASH_BLOCK_SIZE,
		.cra_ctxsize		= sizeof(struct s5p_hash_ctx),
		.cra_alignmask		= SSS_DMA_ALIGN_MASK,
		.cra_module		= THIS_MODULE,
		.cra_init		= s5p_hash_cra_init,
		.cra_exit		= s5p_hash_cra_exit,
	}
}
};

/**
 * struct exynos_hash_algs_info
 */
static struct sss_hash_algs_info exynos_hash_algs_info[] = {
	{
		.algs_list	= algs_sha1_md5,
		.size		= ARRAY_SIZE(algs_sha1_md5),
	},
	{
		.algs_list	= algs_sha256,
		.size		= ARRAY_SIZE(algs_sha256),
	},
};

static void s5p_set_aes(struct s5p_aes_dev *dev,
			uint8_t *key, uint8_t *iv, unsigned int keylen)
{
	void __iomem *keystart;

	if (iv)
		memcpy_toio(dev->aes_ioaddr + SSS_REG_AES_IV_DATA(0), iv, 0x10);

	if (keylen == AES_KEYSIZE_256)
		keystart = dev->aes_ioaddr + SSS_REG_AES_KEY_DATA(0);
	else if (keylen == AES_KEYSIZE_192)
		keystart = dev->aes_ioaddr + SSS_REG_AES_KEY_DATA(2);
	else
		keystart = dev->aes_ioaddr + SSS_REG_AES_KEY_DATA(4);

	memcpy_toio(keystart, key, keylen);
}

static bool s5p_is_sg_aligned(struct scatterlist *sg)
{
	while (sg) {
		if (!IS_ALIGNED(sg->length, AES_BLOCK_SIZE))
			return false;
		sg = sg_next(sg);
	}

	return true;
}

static int s5p_set_indata_start(struct s5p_aes_dev *dev,
				struct ablkcipher_request *req)
{
	struct scatterlist *sg;
	int err;

	dev->sg_src_cpy = NULL;
	sg = req->src;
	if (!s5p_is_sg_aligned(sg)) {
		dev_dbg(dev->dev,
			"At least one unaligned source scatter list, making a copy\n");
		err = s5p_make_sg_cpy(dev, sg, &dev->sg_src_cpy);
		if (err)
			return err;

		sg = dev->sg_src_cpy;
	}

	err = s5p_set_indata(dev, sg);
	if (err) {
		s5p_free_sg_cpy(dev, &dev->sg_src_cpy);
		return err;
	}

	return 0;
}

static int s5p_set_outdata_start(struct s5p_aes_dev *dev,
				struct ablkcipher_request *req)
{
	struct scatterlist *sg;
	int err;

	dev->sg_dst_cpy = NULL;
	sg = req->dst;
	if (!s5p_is_sg_aligned(sg)) {
		dev_dbg(dev->dev,
			"At least one unaligned dest scatter list, making a copy\n");
		err = s5p_make_sg_cpy(dev, sg, &dev->sg_dst_cpy);
		if (err)
			return err;

		sg = dev->sg_dst_cpy;
	}

	err = s5p_set_outdata(dev, sg);
	if (err) {
		s5p_free_sg_cpy(dev, &dev->sg_dst_cpy);
		return err;
	}

	return 0;
}

static void s5p_aes_crypt_start(struct s5p_aes_dev *dev, unsigned long mode)
{
	struct ablkcipher_request *req = dev->req;
	uint32_t aes_control;
	unsigned long flags;
	int err;

	aes_control = SSS_AES_KEY_CHANGE_MODE;
	if (mode & FLAGS_AES_DECRYPT)
		aes_control |= SSS_AES_MODE_DECRYPT;

	if ((mode & FLAGS_AES_MODE_MASK) == FLAGS_AES_CBC)
		aes_control |= SSS_AES_CHAIN_MODE_CBC;
	else if ((mode & FLAGS_AES_MODE_MASK) == FLAGS_AES_CTR)
		aes_control |= SSS_AES_CHAIN_MODE_CTR;

	if (dev->ctx->keylen == AES_KEYSIZE_192)
		aes_control |= SSS_AES_KEY_SIZE_192;
	else if (dev->ctx->keylen == AES_KEYSIZE_256)
		aes_control |= SSS_AES_KEY_SIZE_256;

	aes_control |= SSS_AES_FIFO_MODE;

	/* as a variant it is possible to use byte swapping on DMA side */
	aes_control |= SSS_AES_BYTESWAP_DI
		    |  SSS_AES_BYTESWAP_DO
		    |  SSS_AES_BYTESWAP_IV
		    |  SSS_AES_BYTESWAP_KEY
		    |  SSS_AES_BYTESWAP_CNT;

	spin_lock_irqsave(&dev->lock, flags);

	SSS_WRITE(dev, FCINTENCLR,
		  SSS_FCINTENCLR_BTDMAINTENCLR | SSS_FCINTENCLR_BRDMAINTENCLR);
	SSS_WRITE(dev, FCFIFOCTRL, 0x00);

	err = s5p_set_indata_start(dev, req);
	if (err)
		goto indata_error;

	err = s5p_set_outdata_start(dev, req);
	if (err)
		goto outdata_error;

	SSS_AES_WRITE(dev, AES_CONTROL, aes_control);
	s5p_set_aes(dev, dev->ctx->aes_key, req->info, dev->ctx->keylen);

	s5p_set_dma_indata(dev,  dev->sg_src);
	s5p_set_dma_outdata(dev, dev->sg_dst);

	SSS_WRITE(dev, FCINTENSET,
		  SSS_FCINTENSET_BTDMAINTENSET | SSS_FCINTENSET_BRDMAINTENSET);

	spin_unlock_irqrestore(&dev->lock, flags);

	return;

outdata_error:
	s5p_unset_indata(dev);

indata_error:
	s5p_sg_done(dev);
	dev->busy = false;
	spin_unlock_irqrestore(&dev->lock, flags);
	s5p_aes_complete(dev, err);
}

static void s5p_tasklet_cb(unsigned long data)
{
	struct s5p_aes_dev *dev = (struct s5p_aes_dev *)data;
	struct crypto_async_request *async_req, *backlog;
	struct s5p_aes_reqctx *reqctx;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	backlog   = crypto_get_backlog(&dev->queue);
	async_req = crypto_dequeue_request(&dev->queue);

	if (!async_req) {
		dev->busy = false;
		spin_unlock_irqrestore(&dev->lock, flags);
		return;
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	if (backlog)
		backlog->complete(backlog, -EINPROGRESS);

	dev->req = ablkcipher_request_cast(async_req);
	dev->ctx = crypto_tfm_ctx(dev->req->base.tfm);
	reqctx   = ablkcipher_request_ctx(dev->req);

	s5p_aes_crypt_start(dev, reqctx->mode);
}

static int s5p_aes_handle_req(struct s5p_aes_dev *dev,
			      struct ablkcipher_request *req)
{
	unsigned long flags;
	int err;

	spin_lock_irqsave(&dev->lock, flags);
	err = ablkcipher_enqueue_request(&dev->queue, req);
	if (dev->busy) {
		spin_unlock_irqrestore(&dev->lock, flags);
		goto exit;
	}
	dev->busy = true;

	spin_unlock_irqrestore(&dev->lock, flags);

	tasklet_schedule(&dev->tasklet);

exit:
	return err;
}

static int s5p_aes_crypt(struct ablkcipher_request *req, unsigned long mode)
{
	struct crypto_ablkcipher *tfm = crypto_ablkcipher_reqtfm(req);
	struct s5p_aes_reqctx *reqctx = ablkcipher_request_ctx(req);
	struct s5p_aes_ctx *ctx = crypto_ablkcipher_ctx(tfm);
	struct s5p_aes_dev *dev = ctx->dev;

	if (!IS_ALIGNED(req->nbytes, AES_BLOCK_SIZE)) {
		dev_err(dev->dev, "request size is not exact amount of AES blocks\n");
		return -EINVAL;
	}

	reqctx->mode = mode;

	return s5p_aes_handle_req(dev, req);
}

static int s5p_aes_setkey(struct crypto_ablkcipher *cipher,
			  const uint8_t *key, unsigned int keylen)
{
	struct crypto_tfm *tfm = crypto_ablkcipher_tfm(cipher);
	struct s5p_aes_ctx *ctx = crypto_tfm_ctx(tfm);

	if (keylen != AES_KEYSIZE_128 &&
	    keylen != AES_KEYSIZE_192 &&
	    keylen != AES_KEYSIZE_256)
		return -EINVAL;

	memcpy(ctx->aes_key, key, keylen);
	ctx->keylen = keylen;

	return 0;
}

static int s5p_aes_ecb_encrypt(struct ablkcipher_request *req)
{
	return s5p_aes_crypt(req, 0);
}

static int s5p_aes_ecb_decrypt(struct ablkcipher_request *req)
{
	return s5p_aes_crypt(req, FLAGS_AES_DECRYPT);
}

static int s5p_aes_cbc_encrypt(struct ablkcipher_request *req)
{
	return s5p_aes_crypt(req, FLAGS_AES_CBC);
}

static int s5p_aes_cbc_decrypt(struct ablkcipher_request *req)
{
	return s5p_aes_crypt(req, FLAGS_AES_DECRYPT | FLAGS_AES_CBC);
}

static int s5p_aes_cra_init(struct crypto_tfm *tfm)
{
	struct s5p_aes_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx->dev = s5p_dev;
	tfm->crt_ablkcipher.reqsize = sizeof(struct s5p_aes_reqctx);

	return 0;
}

static struct crypto_alg algs[] = {
	{
		.cra_name		= "ecb(aes)",
		.cra_driver_name	= "ecb-aes-s5p",
		.cra_priority		= 100,
		.cra_flags		= CRYPTO_ALG_TYPE_ABLKCIPHER |
					  CRYPTO_ALG_ASYNC |
					  CRYPTO_ALG_KERN_DRIVER_ONLY,
		.cra_blocksize		= AES_BLOCK_SIZE,
		.cra_ctxsize		= sizeof(struct s5p_aes_ctx),
		.cra_alignmask		= 0x0f,
		.cra_type		= &crypto_ablkcipher_type,
		.cra_module		= THIS_MODULE,
		.cra_init		= s5p_aes_cra_init,
		.cra_u.ablkcipher = {
			.min_keysize	= AES_MIN_KEY_SIZE,
			.max_keysize	= AES_MAX_KEY_SIZE,
			.setkey		= s5p_aes_setkey,
			.encrypt	= s5p_aes_ecb_encrypt,
			.decrypt	= s5p_aes_ecb_decrypt,
		}
	},
	{
		.cra_name		= "cbc(aes)",
		.cra_driver_name	= "cbc-aes-s5p",
		.cra_priority		= 100,
		.cra_flags		= CRYPTO_ALG_TYPE_ABLKCIPHER |
					  CRYPTO_ALG_ASYNC |
					  CRYPTO_ALG_KERN_DRIVER_ONLY,
		.cra_blocksize		= AES_BLOCK_SIZE,
		.cra_ctxsize		= sizeof(struct s5p_aes_ctx),
		.cra_alignmask		= 0x0f,
		.cra_type		= &crypto_ablkcipher_type,
		.cra_module		= THIS_MODULE,
		.cra_init		= s5p_aes_cra_init,
		.cra_u.ablkcipher = {
			.min_keysize	= AES_MIN_KEY_SIZE,
			.max_keysize	= AES_MAX_KEY_SIZE,
			.ivsize		= AES_BLOCK_SIZE,
			.setkey		= s5p_aes_setkey,
			.encrypt	= s5p_aes_cbc_encrypt,
			.decrypt	= s5p_aes_cbc_decrypt,
		}
	},
};

bool use_hash;

static int s5p_aes_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int aes_i, hash_i, hash_algs_size = 0, j, err = -ENODEV;
	struct samsung_aes_variant *variant;
	struct s5p_aes_dev *pdata;
	struct resource *res;
	struct sss_hash_algs_info *hash_algs_i;

	if (s5p_dev)
		return -EEXIST;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	variant = find_s5p_sss_version(pdev);
	pdata->pdata = variant;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	/* HACK: HASH and PRNG uses the same registers in secss,
	 * avoid overwrite each other. This will drop HASH when
	 * CONFIG_EXYNOS_RNG is enabled.
	 * We need larger size for HASH registers in secss, current
	 * describe only AES/DES
	 */
	if (variant == &exynos_aes_data) {
		pdata->pdata->hash_algs_info = exynos_hash_algs_info;
		pdata->pdata->hash_algs_size =
			ARRAY_SIZE(exynos_hash_algs_info);
#ifndef CONFIG_CRYPTO_DEV_EXYNOS_RNG
		res->end += 0x300;
		use_hash = true;
#endif
	}

	pdata->res = res;
	pdata->ioaddr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pdata->ioaddr)) {
		if (!use_hash)
			return PTR_ERR(pdata->ioaddr);
		/* try AES without HASH */
		res->end -= 0x300;
		use_hash = false;
		pdata->ioaddr = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(pdata->ioaddr))
			return PTR_ERR(pdata->ioaddr);
	}

	pdata->clk = devm_clk_get(dev, "secss");
	if (IS_ERR(pdata->clk)) {
		dev_err(dev, "failed to find secss clock source\n");
		return -ENOENT;
	}

	err = clk_prepare_enable(pdata->clk);
	if (err < 0) {
		dev_err(dev, "Enabling SSS clk failed, err %d\n", err);
		return err;
	}

	spin_lock_init(&pdata->lock);
	spin_lock_init(&pdata->hash_lock);

	pdata->aes_ioaddr = pdata->ioaddr + variant->aes_offset;
	pdata->io_hash_base = pdata->ioaddr + variant->hash_offset;

	pdata->irq_fc = platform_get_irq(pdev, 0);
	if (pdata->irq_fc < 0) {
		err = pdata->irq_fc;
		dev_warn(dev, "feed control interrupt is not available.\n");
		goto err_irq;
	}
	err = devm_request_threaded_irq(dev, pdata->irq_fc, NULL,
					s5p_aes_interrupt, IRQF_ONESHOT,
					pdev->name, pdev);
	if (err < 0) {
		dev_warn(dev, "feed control interrupt is not available.\n");
		goto err_irq;
	}

	pdata->busy = false;
	pdata->dev = dev;
	platform_set_drvdata(pdev, pdata);

	s5p_dev = pdata;

	tasklet_init(&pdata->tasklet, s5p_tasklet_cb, (unsigned long)pdata);
	crypto_init_queue(&pdata->queue, CRYPTO_QUEUE_LEN);

	tasklet_init(&pdata->hash_tasklet, s5p_hash_tasklet_cb,
		     (unsigned long)pdata);
	crypto_init_queue(&pdata->hash_queue, SSS_HASH_QUEUE_LENGTH);

	for (aes_i = 0; aes_i < ARRAY_SIZE(algs); aes_i++) {
		err = crypto_register_alg(&algs[aes_i]);
		if (err) {
			dev_err(dev, "can't register '%s': %d\n",
				algs[aes_i].cra_name, err);
			goto err_algs;
		}
	}

	if (use_hash)
		hash_algs_size = pdata->pdata->hash_algs_size;

	for (hash_i = 0; hash_i < hash_algs_size; hash_i++) {
		hash_algs_i = pdata->pdata->hash_algs_info;
		hash_algs_i[hash_i].registered = 0;
		for (j = 0; j < hash_algs_i[hash_i].size; j++) {
			struct ahash_alg *alg;

			alg = &(hash_algs_i[hash_i].algs_list[j]);
			alg->export = s5p_hash_export;
			alg->import = s5p_hash_import;
			alg->halg.statesize = sizeof(struct s5p_hash_reqctx) +
					      BUFLEN;
			err = crypto_register_ahash(alg);
			if (err) {
				dev_err(dev, "can't register '%s': %d\n",
					alg->halg.base.cra_driver_name, err);
				goto err_hash;
			}
			FLOW_LOG("alg registered: %s\n",
				 alg->halg.base.cra_driver_name);

			hash_algs_i[hash_i].registered++;
		}
	}

	dev_info(dev, "s5p-sss driver registered\n");

	return 0;

err_hash:
	for (hash_i = hash_algs_size - 1; hash_i >= 0; hash_i--)
		for (j = hash_algs_i[hash_i].registered - 1;
		     j >= 0; j--)
			crypto_unregister_ahash(
				&(hash_algs_i[hash_i].algs_list[j]));

err_algs:

	for (j = 0; j < aes_i; j++)
		crypto_unregister_alg(&algs[j]);

	tasklet_kill(&pdata->hash_tasklet);
	tasklet_kill(&pdata->tasklet);

err_irq:
	clk_disable_unprepare(pdata->clk);

	s5p_dev = NULL;

	return err;
}

static int s5p_aes_remove(struct platform_device *pdev)
{
	struct s5p_aes_dev *pdata = platform_get_drvdata(pdev);
	struct sss_hash_algs_info *hash_algs_i;
	int i, j;

	if (!pdata)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(algs); i++)
		crypto_unregister_alg(&algs[i]);

	hash_algs_i = pdata->pdata->hash_algs_info;
	for (i = pdata->pdata->hash_algs_size - 1; i >= 0; i--)
		for (j = hash_algs_i[i].registered - 1; j >= 0; j--)
			crypto_unregister_ahash(
				&(hash_algs_i[i].algs_list[j]));

	tasklet_kill(&pdata->hash_tasklet);
	tasklet_kill(&pdata->tasklet);

	clk_disable_unprepare(pdata->clk);
	if (use_hash) {
		pdata->res->end -= 0x300;
		use_hash = false;
	}

	s5p_dev = NULL;

	return 0;
}

static struct platform_driver s5p_aes_crypto = {
	.probe	= s5p_aes_probe,
	.remove	= s5p_aes_remove,
	.driver	= {
		.name	= "s5p-secss",
		.of_match_table = s5p_sss_dt_match,
	},
};

module_platform_driver(s5p_aes_crypto);

MODULE_DESCRIPTION("S5PV210 AES hw acceleration support.");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Vladimir Zapolskiy <vzapolskiy@gmail.com>");
MODULE_AUTHOR("Kamil Konieczny <k.konieczny@partner.samsung.com>");
