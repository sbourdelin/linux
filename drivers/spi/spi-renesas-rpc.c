// SPDX-License-Identifier: GPL-2.0
//
// Copyright (C) 2018 ~ 2019 Renesas Solutions Corp.
// Copyright (C) 2018 Macronix International Co., Ltd.
//
// R-Car Gen3 RPC-IF SPI/QSPI/Octa driver
//
// Authors:
//	Mason Yang <masonccyang@mxic.com.tw>
//

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>

#include <asm/unaligned.h>

#define RPC_CMNCR		0x0000	// R/W
#define RPC_CMNCR_MD		BIT(31)
#define RPC_CMNCR_SFDE		BIT(24) // undocumented bit but must be set
#define RPC_CMNCR_MOIIO3(val)	(((val) & 0x3) << 22)
#define RPC_CMNCR_MOIIO2(val)	(((val) & 0x3) << 20)
#define RPC_CMNCR_MOIIO1(val)	(((val) & 0x3) << 18)
#define RPC_CMNCR_MOIIO0(val)	(((val) & 0x3) << 16)
#define RPC_CMNCR_MOIIO_HIZ	(RPC_CMNCR_MOIIO0(3) | RPC_CMNCR_MOIIO1(3) | \
				 RPC_CMNCR_MOIIO2(3) | RPC_CMNCR_MOIIO3(3))
#define RPC_CMNCR_IO3FV(val)	(((val) & 0x3) << 14) // undocumented bit
#define RPC_CMNCR_IO2FV(val)	(((val) & 0x3) << 12) // undocumented bit
#define RPC_CMNCR_IO0FV(val)	(((val) & 0x3) << 8)
#define RPC_CMNCR_IOFV_HIZ	(RPC_CMNCR_IO0FV(3) | RPC_CMNCR_IO2FV(3) | \
				 RPC_CMNCR_IO3FV(3))
#define RPC_CMNCR_BSZ(val)	(((val) & 0x3) << 0)

#define RPC_SSLDR		0x0004	// R/W
#define RPC_SSLDR_SPNDL(d)	(((d) & 0x7) << 16)
#define RPC_SSLDR_SLNDL(d)	(((d) & 0x7) << 8)
#define RPC_SSLDR_SCKDL(d)	(((d) & 0x7) << 0)

#define RPC_DRCR		0x000C	// R/W
#define RPC_DRCR_SSLN		BIT(24)
#define RPC_DRCR_RBURST(v)	((((v) - 1) & 0x1F) << 16)
#define RPC_DRCR_RCF		BIT(9)
#define RPC_DRCR_RBE		BIT(8)
#define RPC_DRCR_SSLE		BIT(0)

#define RPC_DRCMR		0x0010	// R/W
#define RPC_DRCMR_CMD(c)	(((c) & 0xFF) << 16)
#define RPC_DRCMR_OCMD(c)	(((c) & 0xFF) << 0)

#define RPC_DREAR		0x0014	// R/W
#define RPC_DREAR_EAC(c)	(((c) & 0x7) << 0)

#define RPC_DROPR		0x0018	// R/W

#define RPC_DRENR		0x001C	// R/W
#define RPC_DRENR_CDB(o)	(u32)((((o) & 0x3) << 30))
#define RPC_DRENR_OCDB(o)	(((o) & 0x3) << 28)
#define RPC_DRENR_ADB(o)	(((o) & 0x3) << 24)
#define RPC_DRENR_OPDB(o)	(((o) & 0x3) << 20)
#define RPC_DRENR_SPIDB(o)	(((o) & 0x3) << 16)
#define RPC_DRENR_DME		BIT(15)
#define RPC_DRENR_CDE		BIT(14)
#define RPC_DRENR_OCDE		BIT(12)
#define RPC_DRENR_ADE(v)	(((v) & 0xF) << 8)
#define RPC_DRENR_OPDE(v)	(((v) & 0xF) << 4)

#define RPC_SMCR		0x0020	// R/W
#define RPC_SMCR_SSLKP		BIT(8)
#define RPC_SMCR_SPIRE		BIT(2)
#define RPC_SMCR_SPIWE		BIT(1)
#define RPC_SMCR_SPIE		BIT(0)

#define RPC_SMCMR		0x0024	// R/W
#define RPC_SMCMR_CMD(c)	(((c) & 0xFF) << 16)
#define RPC_SMCMR_OCMD(c)	(((c) & 0xFF) << 0)

#define RPC_SMADR		0x0028	// R/W
#define RPC_SMOPR		0x002C	// R/W
#define RPC_SMOPR_OPD3(o)	(((o) & 0xFF) << 24)
#define RPC_SMOPR_OPD2(o)	(((o) & 0xFF) << 16)
#define RPC_SMOPR_OPD1(o)	(((o) & 0xFF) << 8)
#define RPC_SMOPR_OPD0(o)	(((o) & 0xFF) << 0)

#define RPC_SMENR		0x0030	// R/W
#define RPC_SMENR_CDB(o)	(((o) & 0x2) << 30)
#define RPC_SMENR_OCDB(o)	(((o) & 0x2) << 28)
#define RPC_SMENR_ADB(o)	(((o) & 0x2) << 24)
#define RPC_SMENR_OPDB(o)	(((o) & 0x2) << 20)
#define RPC_SMENR_SPIDB(o)	(((o) & 0x2) << 16)
#define RPC_SMENR_DME		BIT(15)
#define RPC_SMENR_CDE		BIT(14)
#define RPC_SMENR_OCDE		BIT(12)
#define RPC_SMENR_ADE(v)	(((v) & 0xF) << 8)
#define RPC_SMENR_OPDE(v)	(((v) & 0xF) << 4)
#define RPC_SMENR_SPIDE(v)	(((v) & 0xF) << 0)

#define RPC_SMRDR0		0x0038	// R
#define RPC_SMRDR1		0x003C	// R
#define RPC_SMWDR0		0x0040	// W
#define RPC_SMWDR1		0x0044	// W

#define RPC_CMNSR		0x0048	// R
#define RPC_CMNSR_SSLF		BIT(1)
#define RPC_CMNSR_TEND		BIT(0)

#define RPC_DRDMCR		0x0058	// R/W
#define RPC_DRDRENR		0x005C	// R/W

#define RPC_SMDMCR		0x0060	// R/W
#define RPC_SMDMCR_DMCYC(v)	((((v) - 1) & 0x1F) << 0)

#define RPC_SMDRENR		0x0064	// R/W
#define RPC_SMDRENR_HYPE	(0x5 << 12)
#define RPC_SMDRENR_ADDRE	BIT(8)
#define RPC_SMDRENR_OPDRE	BIT(4)
#define RPC_SMDRENR_SPIDRE	BIT(0)

#define RPC_PHYCNT		0x007C	// R/W
#define RPC_PHYCNT_CAL		BIT(31)
#define PRC_PHYCNT_OCTA_AA	BIT(22)
#define PRC_PHYCNT_OCTA_SA	BIT(23)
#define PRC_PHYCNT_EXDS		BIT(21)
#define RPC_PHYCNT_OCT		BIT(20)
#define RPC_PHYCNT_STRTIM(v)	(((v) & 0x7) << 15)
#define RPC_PHYCNT_WBUF2	BIT(4)
#define RPC_PHYCNT_WBUF		BIT(2)
#define RPC_PHYCNT_PHYMEM(v)	(((v) & 0x3) << 0)

#define RPC_PHYOFFSET1		0x0080	// R/W
#define RPC_PHYOFFSET1_DDRTMG(v) (((v) & 0x3) << 28)
#define RPC_PHYOFFSET2		0x0084	// R/W
#define RPC_PHYOFFSET2_OCTTMG(v) (((v) & 0x7) << 8)

#define RPC_WBUF		0x8000	// Write Buffer
#define RPC_WBUF_SIZE		256	// Write Buffer size

struct rpc_spi {
	struct clk *clk_rpc;
	void __iomem *base;
	void __iomem *dirmap;
	struct regmap *regmap;
	u32 cur_speed_hz;
	u32 cmd;
	u32 addr;
	u32 dummy;
	u32 smcr;
	u32 smenr;
	u32 xferlen;
	u32 totalxferlen;
	enum spi_mem_data_dir xfer_dir;
	struct reset_control *rstc;
};

static int rpc_spi_set_freq(struct rpc_spi *rpc, unsigned long freq)
{
	int ret;

	if (rpc->cur_speed_hz == freq)
		return 0;

	ret = clk_set_rate(rpc->clk_rpc, freq);
	if (ret)
		return ret;

	rpc->cur_speed_hz = freq;
	return ret;
}

static void rpc_spi_hw_init(struct rpc_spi *rpc)
{
	//
	// NOTE: The 0x260 are undocumented bits, but they must be set.
	//	RPC_PHYCNT_STRTIM is strobe timing adjustment bit,
	//	0x0 : the delay is biggest,
	//	0x1 : the delay is 2nd biggest,
	//	On H3 ES1.x, the value should be 0, while on others,
	//	the value should be 6.
	//
	regmap_write(rpc->regmap, RPC_PHYCNT, RPC_PHYCNT_CAL |
		     RPC_PHYCNT_STRTIM(6) | 0x260);

	//
	// NOTE: The 0x1511144 are undocumented bits, but they must be set
	//       for RPC_PHYOFFSET1.
	//	 The 0x31 are undocumented bits, but they must be set
	//	 for RPC_PHYOFFSET2.
	//
	regmap_write(rpc->regmap, RPC_PHYOFFSET1,
		     RPC_PHYOFFSET1_DDRTMG(3) | 0x1511144);
	regmap_write(rpc->regmap, RPC_PHYOFFSET2, 0x31 |
		     RPC_PHYOFFSET2_OCTTMG(4));
	regmap_write(rpc->regmap, RPC_SSLDR, RPC_SSLDR_SPNDL(7) |
		     RPC_SSLDR_SLNDL(7) | RPC_SSLDR_SCKDL(7));
	regmap_write(rpc->regmap, RPC_CMNCR, RPC_CMNCR_MD | RPC_CMNCR_SFDE |
		     RPC_CMNCR_MOIIO_HIZ | RPC_CMNCR_IOFV_HIZ |
		     RPC_CMNCR_BSZ(0));
}

static int wait_msg_xfer_end(struct rpc_spi *rpc)
{
	u32 sts;

	return regmap_read_poll_timeout(rpc->regmap, RPC_CMNSR, sts,
					sts & RPC_CMNSR_TEND, 0, USEC_PER_SEC);
}

static u8 rpc_bits_set(u32 nbytes)
{
	nbytes = clamp(nbytes, 1U, 4U);

	return GENMASK(3, 4 - nbytes);
}

static int rpc_spi_io_xfer(struct rpc_spi *rpc,
			   const void *tx_buf, void *rx_buf)
{
	u32 smenr, smcr, data, pos = 0;
	int ret = 0;

	regmap_update_bits(rpc->regmap, RPC_CMNCR, RPC_CMNCR_MD, RPC_CMNCR_MD);
	regmap_write(rpc->regmap, RPC_SMDRENR, 0);
	regmap_write(rpc->regmap, RPC_SMCMR, rpc->cmd);
	regmap_write(rpc->regmap, RPC_SMDMCR, rpc->dummy);
	regmap_write(rpc->regmap, RPC_SMADR, rpc->addr);
	smenr = rpc->smenr;

	if (tx_buf) {
		while (pos < rpc->xferlen) {
			u32 nbytes = rpc->xferlen - pos;

			regmap_write(rpc->regmap, RPC_SMWDR0,
				     get_unaligned((u32 *)(tx_buf + pos)));

			smcr = rpc->smcr | RPC_SMCR_SPIE;

			if (nbytes > 4) {
				nbytes = 4;
				smcr |= RPC_SMCR_SSLKP;
			}

			regmap_write(rpc->regmap, RPC_SMENR, smenr);
			regmap_write(rpc->regmap, RPC_SMCR, smcr);
			ret = wait_msg_xfer_end(rpc);
			if (ret)
				goto out;

			pos += nbytes;
			smenr = rpc->smenr & ~RPC_SMENR_CDE &
					     ~RPC_SMENR_ADE(0xf);
		}
	} else if (rx_buf) {
		//
		// RPC-IF spoils the data for the commands without an address
		// phase (like RDID) in the manual mode, so we'll have to work
		// around this issue by using the external address space read
		// mode instead; we seem to be able to read 8 bytes at most in
		// this mode though...
		//
		if (!(smenr & RPC_SMENR_ADE(0xf))) {
			u32 nbytes = rpc->xferlen - pos;
			u64 tmp;

			if (nbytes > 8)
				nbytes = 8;

			regmap_update_bits(rpc->regmap, RPC_CMNCR,
					   RPC_CMNCR_MD, 0);
			regmap_write(rpc->regmap, RPC_DRCR, 0);
			regmap_write(rpc->regmap, RPC_DREAR, RPC_DREAR_EAC(1));
			regmap_write(rpc->regmap, RPC_DRCMR, rpc->cmd);
			regmap_write(rpc->regmap, RPC_DRDMCR, rpc->dummy);
			regmap_write(rpc->regmap, RPC_DROPR, 0);
			regmap_write(rpc->regmap, RPC_DRENR, rpc->smenr &
				     ~RPC_SMENR_SPIDE(0xf));

			tmp = readq(rpc->dirmap);
			memcpy(rx_buf, &tmp, nbytes);
		} else {
			while (pos < rpc->xferlen) {
				u32 nbytes = rpc->xferlen - pos;

				if (nbytes > 4)
					nbytes = 4;

				regmap_write(rpc->regmap, RPC_SMENR, smenr);
				regmap_write(rpc->regmap, RPC_SMCR, rpc->smcr |
					     RPC_SMCR_SPIE);
				ret = wait_msg_xfer_end(rpc);
				if (ret)
					goto out;

				regmap_read(rpc->regmap, RPC_SMRDR0, &data);
				memcpy(rx_buf + pos, &data, nbytes);
				pos += nbytes;

				regmap_write(rpc->regmap, RPC_SMADR,
					     rpc->addr + pos);
			}
		}
	} else {
		regmap_write(rpc->regmap, RPC_SMENR, rpc->smenr);
		regmap_write(rpc->regmap, RPC_SMCR, rpc->smcr | RPC_SMCR_SPIE);
		ret = wait_msg_xfer_end(rpc);
		if (ret)
			goto out;
	}

	return ret;

out:
	return reset_control_reset(rpc->rstc);
}

static void rpc_spi_mem_set_prep_op_cfg(struct spi_device *spi,
					const struct spi_mem_op *op,
					u64 *offs, size_t *len)
{
	struct rpc_spi *rpc = spi_master_get_devdata(spi->master);

	rpc->cmd = RPC_SMCMR_CMD(op->cmd.opcode);
	rpc->smenr = RPC_SMENR_CDE |
		     RPC_SMENR_CDB(ilog2(op->cmd.buswidth));
	rpc->totalxferlen = 1;
	rpc->xfer_dir = SPI_MEM_NO_DATA;
	rpc->xferlen = 0;
	rpc->addr = 0;

	if (op->addr.nbytes) {
		rpc->smenr |= RPC_SMENR_ADB(ilog2(op->addr.buswidth));
		if (op->addr.nbytes == 4)
			rpc->smenr |= RPC_SMENR_ADE(0xf);
		else
			rpc->smenr |= RPC_SMENR_ADE(0x7);

		if (offs && len)
			rpc->addr = *offs;
		else
			rpc->addr = op->addr.val;
		rpc->totalxferlen += op->addr.nbytes;
	}

	if (op->dummy.nbytes) {
		rpc->smenr |= RPC_SMENR_DME;
		rpc->dummy = RPC_SMDMCR_DMCYC(op->dummy.nbytes);
		rpc->totalxferlen += op->dummy.nbytes;
	}

	if (op->data.nbytes || (offs && len)) {
		if (op->data.dir == SPI_MEM_DATA_IN) {
			rpc->smcr = RPC_SMCR_SPIRE;
			rpc->xfer_dir = SPI_MEM_DATA_IN;
		} else if (op->data.dir == SPI_MEM_DATA_OUT) {
			rpc->smcr = RPC_SMCR_SPIWE;
			rpc->xfer_dir = SPI_MEM_DATA_OUT;
		}

		if (offs && len) {
			rpc->smenr |= RPC_SMENR_SPIDE(rpc_bits_set(*len)) |
				      RPC_SMENR_SPIDB(ilog2(op->data.buswidth));
			rpc->xferlen = *len;
			rpc->totalxferlen += *len;
		} else {
			rpc->smenr |=
				RPC_SMENR_SPIDE(rpc_bits_set(op->data.nbytes)) |
				RPC_SMENR_SPIDB(ilog2(op->data.buswidth));
			rpc->xferlen = op->data.nbytes;
			rpc->totalxferlen += op->data.nbytes;
		}
	}
}

static bool rpc_spi_mem_supports_op(struct spi_mem *mem,
				    const struct spi_mem_op *op)
{
	if (op->data.buswidth > 4 || op->addr.buswidth > 4 ||
	    op->dummy.buswidth > 4 || op->cmd.buswidth > 4 ||
	    op->addr.nbytes > 4)
		return false;

	return true;
}

static ssize_t rpc_spi_mem_dirmap_read(struct spi_mem_dirmap_desc *desc,
				       u64 offs, size_t len, void *buf)
{
	struct rpc_spi *rpc = spi_master_get_devdata(desc->mem->spi->master);
	int ret;

	if (WARN_ON(offs + desc->info.offset + len > U32_MAX))
		return -EINVAL;

	if (WARN_ON(len > 0x4000000))
		len = 0x4000000;

	ret = rpc_spi_set_freq(rpc, desc->mem->spi->max_speed_hz);
	if (ret)
		return ret;

	rpc_spi_mem_set_prep_op_cfg(desc->mem->spi,
				    &desc->info.op_tmpl, &offs, &len);

	regmap_update_bits(rpc->regmap, RPC_CMNCR, RPC_CMNCR_MD, 0);
	regmap_write(rpc->regmap, RPC_DRCR, RPC_DRCR_RBURST(32) |
		     RPC_DRCR_RBE);
	regmap_write(rpc->regmap, RPC_DRCMR, rpc->cmd);
	regmap_write(rpc->regmap, RPC_DREAR, RPC_DREAR_EAC(1));
	regmap_write(rpc->regmap, RPC_DROPR, 0);
	regmap_write(rpc->regmap, RPC_DRENR, rpc->smenr);
	regmap_write(rpc->regmap, RPC_DRDMCR, rpc->dummy);
	regmap_write(rpc->regmap, RPC_DRDRENR, 0);

	memcpy_fromio(buf, rpc->dirmap + desc->info.offset + offs, len);

	return len;
}

static ssize_t rpc_spi_mem_dirmap_write(struct spi_mem_dirmap_desc *desc,
					u64 offs, size_t len, const void *buf)
{
	struct rpc_spi *rpc = spi_master_get_devdata(desc->mem->spi->master);
	int ret;

	if (WARN_ON(offs + desc->info.offset + len > U32_MAX))
		return -EINVAL;

	if (WARN_ON(len > RPC_WBUF_SIZE))
		len = RPC_WBUF_SIZE;

	ret = rpc_spi_set_freq(rpc, desc->mem->spi->max_speed_hz);
	if (ret)
		return ret;

	rpc_spi_mem_set_prep_op_cfg(desc->mem->spi,
				    &desc->info.op_tmpl, &offs, &len);

	regmap_update_bits(rpc->regmap, RPC_CMNCR, RPC_CMNCR_MD, RPC_CMNCR_MD);

	regmap_write(rpc->regmap, RPC_SMDRENR, 0);
	regmap_write(rpc->regmap, RPC_PHYCNT, RPC_PHYCNT_CAL | 0x260 |
				  RPC_PHYCNT_WBUF2 | RPC_PHYCNT_WBUF);

	memcpy_toio(rpc->base + RPC_WBUF, buf, len);

	regmap_write(rpc->regmap, RPC_SMCMR, rpc->cmd);
	regmap_write(rpc->regmap, RPC_SMADR, offs);
	regmap_write(rpc->regmap, RPC_SMENR, rpc->smenr);
	regmap_write(rpc->regmap, RPC_SMCR, rpc->smcr | RPC_SMCR_SPIE);
	ret = wait_msg_xfer_end(rpc);
	if (ret)
		goto out;

	regmap_write(rpc->regmap, RPC_DRCR, RPC_DRCR_RCF);
	regmap_write(rpc->regmap, RPC_PHYCNT, RPC_PHYCNT_CAL |
				  RPC_PHYCNT_STRTIM(6) | 0x260);

	return len;

out:
	return reset_control_reset(rpc->rstc);
}

static int rpc_spi_mem_dirmap_create(struct spi_mem_dirmap_desc *desc)
{
	struct rpc_spi *rpc = spi_master_get_devdata(desc->mem->spi->master);

	if (desc->info.offset + desc->info.length > U32_MAX)
		return -ENOTSUPP;

	if (!rpc_spi_mem_supports_op(desc->mem, &desc->info.op_tmpl))
		return -ENOTSUPP;

	if (!rpc->dirmap &&
	    desc->info.op_tmpl.data.dir == SPI_MEM_DATA_IN)
		return -ENOTSUPP;

	return 0;
}

static int rpc_spi_mem_exec_op(struct spi_mem *mem,
			       const struct spi_mem_op *op)
{
	struct rpc_spi *rpc = spi_master_get_devdata(mem->spi->master);
	int ret;

	ret = rpc_spi_set_freq(rpc, mem->spi->max_speed_hz);
	if (ret)
		return ret;

	rpc_spi_mem_set_prep_op_cfg(mem->spi, op, NULL, NULL);

	ret = rpc_spi_io_xfer(rpc,
			      op->data.dir == SPI_MEM_DATA_OUT ?
			      op->data.buf.out : NULL,
			      op->data.dir == SPI_MEM_DATA_IN ?
			      op->data.buf.in : NULL);

	return ret;
}

static const struct spi_controller_mem_ops rpc_spi_mem_ops = {
	.supports_op = rpc_spi_mem_supports_op,
	.exec_op = rpc_spi_mem_exec_op,
	.dirmap_create = rpc_spi_mem_dirmap_create,
	.dirmap_read = rpc_spi_mem_dirmap_read,
	.dirmap_write = rpc_spi_mem_dirmap_write,
};

static void rpc_spi_transfer_setup(struct rpc_spi *rpc,
				   struct spi_message *msg)
{
	struct spi_transfer *t, xfer[4] = { };
	u32 i, xfercnt, xferpos = 0;

	rpc->totalxferlen = 0;
	rpc->xfer_dir = SPI_MEM_NO_DATA;

	list_for_each_entry(t, &msg->transfers, transfer_list) {
		if (t->tx_buf) {
			xfer[xferpos].tx_buf = t->tx_buf;
			xfer[xferpos].tx_nbits = t->tx_nbits;
		}

		if (t->rx_buf) {
			xfer[xferpos].rx_buf = t->rx_buf;
			xfer[xferpos].rx_nbits = t->rx_nbits;
		}

		if (t->len) {
			xfer[xferpos++].len = t->len;
			rpc->totalxferlen += t->len;
		}

		if (list_is_last(&t->transfer_list, &msg->transfers)) {
			if (xferpos > 1) {
				if (t->rx_buf) {
					rpc->xfer_dir = SPI_MEM_DATA_IN;
					rpc->smcr = RPC_SMCR_SPIRE;
				} else if (t->tx_buf) {
					rpc->xfer_dir = SPI_MEM_DATA_OUT;
					rpc->smcr = RPC_SMCR_SPIWE;
				}
			}
		}
	}

	xfercnt = xferpos;
	rpc->xferlen = xfer[--xferpos].len;
	rpc->cmd = RPC_SMCMR_CMD(((u8 *)xfer[0].tx_buf)[0]);
	rpc->smenr = RPC_SMENR_CDE |
		     RPC_SMENR_CDB(ilog2((unsigned int)xfer[0].tx_nbits));
	rpc->addr = 0;

	if (xfercnt > 2 && xfer[1].len && xfer[1].tx_buf) {
		rpc->smenr |=
			RPC_SMENR_ADB(ilog2((unsigned int)xfer[1].tx_nbits));

		for (i = 0; i < xfer[1].len; i++)
			rpc->addr |= ((u8 *)xfer[1].tx_buf)[i] <<
				     (8 * (xfer[1].len - i - 1));

		if (xfer[1].len == 4)
			rpc->smenr |= RPC_SMENR_ADE(0xf);
		else
			rpc->smenr |= RPC_SMENR_ADE(0x7);
	}

	if (xfercnt > 3 && xfer[2].len && xfer[2].tx_buf) {
		rpc->smenr |= RPC_SMENR_DME;
		rpc->dummy = RPC_SMDMCR_DMCYC(xfer[2].len);
	}

	for (i = xfercnt - 1; i < xfercnt && xfercnt > 1; i++) {
		if (xfer[i].rx_buf) {
			rpc->smenr |=
				RPC_SMENR_SPIDE(rpc_bits_set(xfer[i].len)) |
				RPC_SMENR_SPIDB
					(ilog2((unsigned int)xfer[i].rx_nbits));
		} else if (xfer[i].tx_buf) {
			rpc->smenr |=
				RPC_SMENR_SPIDE(rpc_bits_set(xfer[i].len)) |
				RPC_SMENR_SPIDB
					(ilog2((unsigned int)xfer[i].tx_nbits));
		}
	}
}

static int rpc_spi_xfer_message(struct rpc_spi *rpc, struct spi_transfer *t)
{
	int ret;

	ret = rpc_spi_set_freq(rpc, t->speed_hz);
	if (ret)
		return ret;

	ret = rpc_spi_io_xfer(rpc,
			      rpc->xfer_dir == SPI_MEM_DATA_OUT ?
			      t->tx_buf : NULL,
			      rpc->xfer_dir == SPI_MEM_DATA_IN ?
			      t->rx_buf : NULL);

	return ret;
}

static int rpc_spi_transfer_one_message(struct spi_master *master,
					struct spi_message *msg)
{
	struct rpc_spi *rpc = spi_master_get_devdata(master);
	struct spi_transfer *t;
	int ret;

	rpc_spi_transfer_setup(rpc, msg);

	list_for_each_entry(t, &msg->transfers, transfer_list) {
		if (!list_is_last(&t->transfer_list, &msg->transfers))
			continue;
		ret = rpc_spi_xfer_message(rpc, t);
		if (ret)
			goto out;
	}

	msg->status = 0;
	msg->actual_length = rpc->totalxferlen;
out:
	spi_finalize_current_message(master);
	return 0;
}

static const struct regmap_range rpc_spi_volatile_ranges[] = {
	regmap_reg_range(RPC_SMRDR0, RPC_SMRDR0),
	regmap_reg_range(RPC_SMWDR0, RPC_SMWDR0),
	regmap_reg_range(RPC_CMNSR, RPC_CMNSR),
};

static const struct regmap_access_table rpc_spi_volatile_table = {
	.yes_ranges	= rpc_spi_volatile_ranges,
	.n_yes_ranges	= ARRAY_SIZE(rpc_spi_volatile_ranges),
};

static const struct regmap_config rpc_spi_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.fast_io = true,
	.max_register = RPC_PHYOFFSET2,
	.volatile_table = &rpc_spi_volatile_table,
};

static int rpc_spi_probe(struct platform_device *pdev)
{
	struct spi_master *master;
	struct resource *res;
	struct rpc_spi *rpc;
	const struct regmap_config *regmap_config;
	const char *mode;
	int ret;

	ret = of_property_read_string(pdev->dev.of_node,
				      "renesas,rpc-mode", &mode);
	if (ret < 0)
		return ret;

	if (strcasecmp(mode, "spi"))
		return -ENODEV;

	master = spi_alloc_master(&pdev->dev, sizeof(*rpc));
	if (!master)
		return -ENOMEM;

	platform_set_drvdata(pdev, master);

	rpc = spi_master_get_devdata(master);

	master->dev.of_node = pdev->dev.of_node;

	rpc->clk_rpc = devm_clk_get(&pdev->dev, "rpc");
	if (IS_ERR(rpc->clk_rpc))
		return PTR_ERR(rpc->clk_rpc);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "regs");
	rpc->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(rpc->base))
		return PTR_ERR(rpc->base);

	regmap_config = &rpc_spi_regmap_config;
	rpc->regmap = devm_regmap_init_mmio(&pdev->dev, rpc->base,
					    regmap_config);
	if (IS_ERR(rpc->regmap)) {
		dev_err(&pdev->dev, "failed to init regmap %ld for rpc-spi\n",
			PTR_ERR(rpc->regmap));
		return PTR_ERR(rpc->regmap);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dirmap");
	rpc->dirmap = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(rpc->dirmap))
		rpc->dirmap = NULL;

	rpc->rstc = devm_reset_control_get_exclusive(&pdev->dev, NULL);
	if (IS_ERR(rpc->rstc))
		return PTR_ERR(rpc->rstc);

	pm_runtime_enable(&pdev->dev);
	master->auto_runtime_pm = true;

	master->num_chipselect = 1;
	master->mem_ops = &rpc_spi_mem_ops;
	master->transfer_one_message = rpc_spi_transfer_one_message;

	master->bits_per_word_mask = SPI_BPW_MASK(8);
	master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_TX_QUAD | SPI_RX_QUAD;

	rpc_spi_hw_init(rpc);

	ret = spi_register_master(master);
	if (ret) {
		dev_err(&pdev->dev, "spi_register_master failed\n");
		goto err_put_master;
	}
	return 0;

err_put_master:
	spi_master_put(master);
	pm_runtime_disable(&pdev->dev);

	return ret;
}

static int rpc_spi_remove(struct platform_device *pdev)
{
	struct spi_master *master = platform_get_drvdata(pdev);

	pm_runtime_disable(&pdev->dev);
	spi_unregister_master(master);

	return 0;
}

static const struct of_device_id rpc_spi_of_ids[] = {
	{ .compatible = "renesas,r8a77995-rpc", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rpc_spi_of_ids);

#ifdef CONFIG_PM_SLEEP
static int rpc_spi_suspend(struct device *dev)
{
	struct spi_master *master = dev_get_drvdata(dev);

	return spi_master_suspend(master);
}

static int rpc_spi_resume(struct device *dev)
{
	struct spi_master *master = dev_get_drvdata(dev);

	return spi_master_resume(master);
}

static SIMPLE_DEV_PM_OPS(rpc_spi_pm_ops, rpc_spi_suspend, rpc_spi_resume);
#define DEV_PM_OPS	(&rpc_spi_pm_ops)
#else
#define DEV_PM_OPS	NULL
#endif

static struct platform_driver rpc_spi_driver = {
	.probe = rpc_spi_probe,
	.remove = rpc_spi_remove,
	.driver = {
		.name = "rpc-spi",
		.of_match_table = rpc_spi_of_ids,
		.pm = DEV_PM_OPS,
	},
};
module_platform_driver(rpc_spi_driver);

MODULE_AUTHOR("Mason Yang <masonccyang@mxic.com.tw>");
MODULE_DESCRIPTION("Renesas R-Car Gen3 RPC-IF SPI controller driver");
MODULE_LICENSE("GPL v2");
