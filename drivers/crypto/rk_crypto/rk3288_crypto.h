#ifndef __RK3288_CRYPTO_H__
#define __RK3288_CRYPTO_H__

#include <crypto/sha.h>
#include <crypto/internal/hash.h>
#include <crypto/aes.h>
#include <crypto/des.h>
#include <crypto/ctr.h>
#include <crypto/algapi.h>
#include <linux/interrupt.h>
#include <linux/delay.h>

#define _SBF(s, v)			((v) << (s))
#define _BIT(b)				_SBF(b, 1)

#define FLAGS_HASH_SHA1			_SBF(2, 0x00)
#define FLAGS_HASH_MD5			_SBF(2, 0x01)
#define FLAGS_HASH_SHA256		_SBF(2, 0x02)
#define FLAGS_HASH_PRNG			_SBF(2, 0x03)

/* Crypto control registers*/
#define RK_CRYPTO_INTSTS		0x0000
#define RK_CRYPTO_PKA_DONE_INT		_BIT(5)
#define RK_CRYPTO_HASH_DONE_INT		_BIT(4)
#define RK_CRYPTO_HRDMA_ERR_INT		_BIT(3)
#define RK_CRYPTO_HRDMA_DONE_INT	_BIT(2)
#define RK_CRYPTO_BCDMA_ERR_INT		_BIT(1)
#define RK_CRYPTO_BCDMA_DONE_INT	_BIT(0)

#define RK_CRYPTO_INTENA		0x0004
#define RK_CRYPTO_PKA_DONE_ENA		_BIT(5)
#define RK_CRYPTO_HASH_DONE_ENA		_BIT(4)
#define RK_CRYPTO_HRDMA_ERR_ENA		_BIT(3)
#define RK_CRYPTO_HRDMA_DONE_ENA	_BIT(2)
#define RK_CRYPTO_BCDMA_ERR_ENA		_BIT(1)
#define RK_CRYPTO_BCDMA_DONE_ENA	_BIT(0)

#define RK_CRYPTO_CTRL			0x0008
#define RK_CRYPTO_WRITE_MASK		(0xFFFF<<16)
#define RK_CRYPTO_TRNG_FLUSH		_BIT(9)
#define RK_CRYPTO_TRNG_START		_BIT(8)
#define RK_CRYPTO_PKA_FLUSH		_BIT(7)
#define RK_CRYPTO_HASH_FLUSH		_BIT(6)
#define RK_CRYPTO_BLOCK_FLUSH		_BIT(5)
#define RK_CRYPTO_PKA_START		_BIT(4)
#define RK_CRYPTO_HASH_START		_BIT(3)
#define RK_CRYPTO_BLOCK_START		_BIT(2)
#define RK_CRYPTO_TDES_START		_BIT(1)
#define RK_CRYPTO_AES_START		_BIT(0)

#define RK_CRYPTO_CONF			0x000c
/* HASH Receive DMA Address Mode:   fix | increment */
#define RK_CRYPTO_HR_ADDR_MODE		_BIT(8)
/* Block Transmit DMA Address Mode: fix | increment */
#define RK_CRYPTO_BT_ADDR_MODE		_BIT(7)
/* Block Receive DMA Address Mode:  fix | increment */
#define RK_CRYPTO_BR_ADDR_MODE		_BIT(6)
#define RK_CRYPTO_BYTESWAP_HRFIFO	_BIT(5)
#define RK_CRYPTO_BYTESWAP_BTFIFO	_BIT(4)
#define RK_CRYPTO_BYTESWAP_BRFIFO	_BIT(3)
/* AES = 0 OR DES = 1 */
#define RK_CRYPTO_DESSEL				_BIT(2)
#define RK_CYYPTO_HASHINSEL_INDEPENDENT_SOURCE		_SBF(0, 0x00)
#define RK_CYYPTO_HASHINSEL_BLOCK_CIPHER_INPUT		_SBF(0, 0x01)
#define RK_CYYPTO_HASHINSEL_BLOCK_CIPHER_OUTPUT		_SBF(0, 0x02)

/* Block Receiving DMA Start Address Register */
#define RK_CRYPTO_BRDMAS		0x0010
/* Block Transmitting DMA Start Address Register */
#define RK_CRYPTO_BTDMAS		0x0014
/* Block Receiving DMA Length Register */
#define RK_CRYPTO_BRDMAL		0x0018
/* Hash Receiving DMA Start Address Register */
#define RK_CRYPTO_HRDMAS		0x001c
/* Hash Receiving DMA Length Register */
#define RK_CRYPTO_HRDMAL		0x0020

/* AES registers */
#define RK_CRYPTO_AES_CTRL			  0x0080
#define RK_CRYPTO_AES_BYTESWAP_CNT	_BIT(11)
#define RK_CRYPTO_AES_BYTESWAP_KEY	_BIT(10)
#define RK_CRYPTO_AES_BYTESWAP_IV	_BIT(9)
#define RK_CRYPTO_AES_BYTESWAP_DO	_BIT(8)
#define RK_CRYPTO_AES_BYTESWAP_DI	_BIT(7)
#define RK_CRYPTO_AES_KEY_CHANGE	_BIT(6)
#define RK_CRYPTO_AES_ECB_MODE		_SBF(4, 0x00)
#define RK_CRYPTO_AES_CBC_MODE		_SBF(4, 0x01)
#define RK_CRYPTO_AES_CTR_MODE		_SBF(4, 0x02)
#define RK_CRYPTO_AES_128_bit_key	_SBF(2, 0x00)
#define RK_CRYPTO_AES_192_bit_key	_SBF(2, 0x01)
#define RK_CRYPTO_AES_256_bit_key	_SBF(2, 0x02)
/* Slave = 0 / fifo = 1 */
#define RK_CRYPTO_AES_FIFO_MODE		_BIT(1)
/* Encryption = 0 , Decryption = 1 */
#define RK_CRYPTO_AES_DEC		_BIT(0)

#define RK_CRYPTO_AES_STS		0x0084
#define RK_CRYPTO_AES_DONE		_BIT(0)

/* AES Input Data 0-3 Register */
#define RK_CRYPTO_AES_DIN_0		0x0088
#define RK_CRYPTO_AES_DIN_1		0x008c
#define RK_CRYPTO_AES_DIN_2		0x0090
#define RK_CRYPTO_AES_DIN_3		0x0094

/* AES output Data 0-3 Register */
#define RK_CRYPTO_AES_DOUT_0		0x0098
#define RK_CRYPTO_AES_DOUT_1		0x009c
#define RK_CRYPTO_AES_DOUT_2		0x00a0
#define RK_CRYPTO_AES_DOUT_3		0x00a4

/* AES IV Data 0-3 Register */
#define RK_CRYPTO_AES_IV_0		0x00a8
#define RK_CRYPTO_AES_IV_1		0x00ac
#define RK_CRYPTO_AES_IV_2		0x00b0
#define RK_CRYPTO_AES_IV_3		0x00b4

/* AES Key Data 0-3 Register */
#define RK_CRYPTO_AES_KEY_0		0x00b8
#define RK_CRYPTO_AES_KEY_1		0x00bc
#define RK_CRYPTO_AES_KEY_2		0x00c0
#define RK_CRYPTO_AES_KEY_3		0x00c4
#define RK_CRYPTO_AES_KEY_4		0x00c8
#define RK_CRYPTO_AES_KEY_5		0x00cc
#define RK_CRYPTO_AES_KEY_6		0x00d0
#define RK_CRYPTO_AES_KEY_7		0x00d4

/* AES Input Counter 0-3 Register */
#define RK_CRYPTO_AES_CNT_0		0x00d8
#define RK_CRYPTO_AES_CNT_1		0x00dc
#define RK_CRYPTO_AES_CNT_2		0x00e0
#define RK_CRYPTO_AES_CNT_3		0x00e4

/* des/tdes */
#define RK_CRYPTO_TDES_CTRL		0x0100
#define RK_CRYPTO_TDES_BYTESWAP_KEY	_BIT(8)
#define RK_CRYPTO_TDES_BYTESWAP_IV	_BIT(7)
#define RK_CRYPTO_TDES_BYTESWAP_DO	_BIT(6)
#define RK_CRYPTO_TDES_BYTESWAP_DI	_BIT(5)
/* 0: ECB, 1: CBC */
#define RK_CRYPTO_TDES_CHAINMODE	_BIT(4)
/* TDES Key Mode, 0 : EDE, 1 : EEE */
#define RK_CRYPTO_TDES_EEE		_BIT(3)
/* 0: DES, 1:TDES */
#define RK_CRYPTO_TDES_SELECT		_BIT(2)
/* 0: Slave, 1:Fifo */
#define RK_CRYPTO_TDES_FIFO_MODE	_BIT(1)
/* Encryption = 0 , Decryption = 1 */
#define RK_CRYPTO_TDES_DEC		_BIT(0)

#define RK_CRYPTO_TDES_STS		0x0104
#define RK_CRYPTO_TDES_DONE		_BIT(0)

#define RK_CRYPTO_TDES_DIN_0		0x0108
#define RK_CRYPTO_TDES_DIN_1		0x010c
#define RK_CRYPTO_TDES_DOUT_0		0x0110
#define RK_CRYPTO_TDES_DOUT_1		0x0114
#define RK_CRYPTO_TDES_IV_0		0x0118
#define RK_CRYPTO_TDES_IV_1		0x011c
#define RK_CRYPTO_TDES_KEY1_0		0x0120
#define RK_CRYPTO_TDES_KEY1_1		0x0124
#define RK_CRYPTO_TDES_KEY2_0		0x0128
#define RK_CRYPTO_TDES_KEY2_1		0x012c
#define RK_CRYPTO_TDES_KEY3_0		0x0130
#define RK_CRYPTO_TDES_KEY3_1		0x0134

/* HASH */
#define RK_CRYPTO_HASH_CTRL		0x0180
#define RK_CRYPTO_HASH_SWAP_DO		_BIT(3)
#define RK_CRYPTO_HASH_SWAP_DI		_BIT(2)
#define RK_CRYPTO_HASH_SHA1		_SBF(0, 0x00)
#define RK_CRYPTO_HASH_MD5		_SBF(0, 0x01)
#define RK_CRYPTO_HASH_SHA256		_SBF(0, 0x02)
#define RK_CRYPTO_HASH_PRNG		_SBF(0, 0x03)

#define RK_CRYPTO_HASH_STS		0x0184
#define RK_CRYPTO_HASH_DONE		_BIT(0)

#define RK_CRYPTO_HASH_MSG_LEN		0x0188
#define RK_CRYPTO_HASH_DOUT_0		0x018c
#define RK_CRYPTO_HASH_DOUT_1		0x0190
#define RK_CRYPTO_HASH_DOUT_2		0x0194
#define RK_CRYPTO_HASH_DOUT_3		0x0198
#define RK_CRYPTO_HASH_DOUT_4		0x019c
#define RK_CRYPTO_HASH_DOUT_5		0x01a0
#define RK_CRYPTO_HASH_DOUT_6		0x01a4
#define RK_CRYPTO_HASH_DOUT_7		0x01a8
#define RK_CRYPTO_HASH_SEED_0		0x01ac
#define RK_CRYPTO_HASH_SEED_1		0x01b0
#define RK_CRYPTO_HASH_SEED_2		0x01b4
#define RK_CRYPTO_HASH_SEED_3		0x01b8
#define RK_CRYPTO_HASH_SEED_4		0x01bc

/* TRNG */
#define RK_CRYPTO_TRNG_CTRL		0x0200
#define RK_CRYPTO_OSC_ENABLE		_BIT(16)

#define RK_CRYPTO_TRNG_DOUT_0		0x0204
#define RK_CRYPTO_TRNG_DOUT_1		0x0208
#define RK_CRYPTO_TRNG_DOUT_2		0x020c
#define RK_CRYPTO_TRNG_DOUT_3		0x0210
#define RK_CRYPTO_TRNG_DOUT_4		0x0214
#define RK_CRYPTO_TRNG_DOUT_5		0x0218
#define RK_CRYPTO_TRNG_DOUT_6		0x021c
#define RK_CRYPTO_TRNG_DOUT_7		0x0220

/* PAK OR RSA */
#define RK_CRYPTO_PKA_CTRL			0x0280
#define RK_CRYPTO_PKA_BLOCK_SIZE_512BIT		_SBF(0, 0x00)
#define RK_CRYPTO_PKA_BLOCK_SIZE_1024BIT	_SBF(0, 0x01)
#define RK_CRYPTO_PKA_BLOCK_SIZE_2048BIT	_SBF(0, 0x02)

/* result = (M ^ E) mod N */
#define RK_CRYPTO_PKA_M			0x0400
/* C = 2 ^ (2n+2) mod N */
#define RK_CRYPTO_PKA_C			0x0500
#define RK_CRYPTO_PKA_N			0x0600
#define RK_CRYPTO_PKA_E			0x0700

#define CRYPTO_READ(dev, offset)		  \
		__raw_readl(((dev)->reg + (offset)))
#define CRYPTO_WRITE(dev, offset, val)	  \
		__raw_writel((val), ((dev)->reg + (offset)))
/* get register virt address */
#define CRYPTO_GET_REG_VIRT(dev, offset)   ((dev)->reg + (offset))

#define MD5_DIGEST_SIZE			16
#define RK_ALIGN_MASK			(sizeof(u32)-1)

struct crypto_info_t {
	struct device			*dev;
	struct clk			*aclk;
	struct clk			*hclk;
	struct clk			*clk;
	struct clk			*pclk;
	void __iomem			*reg;
	int				irq;
	struct crypto_queue		queue;
	struct tasklet_struct		crypto_tasklet;
	struct ahash_request		*ahash_req;
	struct ablkcipher_request	*ablk_req;
	spinlock_t			lock;

	/* the public variable */
	struct scatterlist		*sg_src;
	struct scatterlist		*sg_dst;
	struct scatterlist		sg_tmp;
	struct scatterlist		*first;
	unsigned int			left_bytes;
	char				*addr_vir;
	int				aligned;
	int				align_size;
	size_t				nents;
	unsigned int			total;
	uint32_t			count;
	uint32_t			mode;
	dma_addr_t			addr_in;
	dma_addr_t			addr_out;
	int (*start)(struct crypto_info_t *dev);
	int (*update)(struct crypto_info_t *dev);
	void (*complete)(struct crypto_info_t *dev, int err);
	int (*enable_clk)(struct crypto_info_t *dev);
	void (*disable_clk)(struct crypto_info_t *dev);
	int (*load_data)(struct crypto_info_t *dev,
			  struct scatterlist *sg_src,
			  struct scatterlist *sg_dst);
	void (*unload_data)(struct crypto_info_t *dev);
};

/* the private variable of hash */
struct rk_ahash_ctx {
	struct crypto_info_t		*dev;
	int				FLAG_FINUP;
	int				first_op;
};

/* the private variable of cipher */
struct rk_cipher_ctx {
	struct crypto_info_t		*dev;
	int				keylen;
};
extern struct crypto_info_t *crypto_p;

extern struct crypto_alg rk_ecb_aes_alg;
extern struct crypto_alg rk_cbc_aes_alg;
extern struct crypto_alg rk_ecb_des_alg;
extern struct crypto_alg rk_cbc_des_alg;
extern struct crypto_alg rk_ecb_des3_ede_alg;
extern struct crypto_alg rk_cbc_des3_ede_alg;

#endif
