// SPDX-License-Identifier: GPL-2.0
// drivers/jtag/aspeed-jtag.c
//
// Copyright (c) 2018 Mellanox Technologies. All rights reserved.
// Copyright (c) 2018 Oleksandr Shamray <oleksandrs@mellanox.com>

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/jtag.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <uapi/linux/jtag.h>

#define ASPEED_JTAG_DATA		0x00
#define ASPEED_JTAG_INST		0x04
#define ASPEED_JTAG_CTRL		0x08
#define ASPEED_JTAG_ISR			0x0C
#define ASPEED_JTAG_SW			0x10
#define ASPEED_JTAG_TCK			0x14
#define ASPEED_JTAG_EC			0x18

#define ASPEED_JTAG_DATA_MSB		0x01
#define ASPEED_JTAG_DATA_CHUNK_SIZE	0x20

/* ASPEED_JTAG_CTRL: Engine Control */
#define ASPEED_JTAG_CTL_ENG_EN		BIT(31)
#define ASPEED_JTAG_CTL_ENG_OUT_EN	BIT(30)
#define ASPEED_JTAG_CTL_FORCE_TMS	BIT(29)
#define ASPEED_JTAG_CTL_INST_LEN(x)	((x) << 20)
#define ASPEED_JTAG_CTL_LASPEED_INST	BIT(17)
#define ASPEED_JTAG_CTL_INST_EN		BIT(16)
#define ASPEED_JTAG_CTL_DR_UPDATE	BIT(10)
#define ASPEED_JTAG_CTL_DATA_LEN(x)	((x) << 4)
#define ASPEED_JTAG_CTL_LASPEED_DATA	BIT(1)
#define ASPEED_JTAG_CTL_DATA_EN		BIT(0)

/* ASPEED_JTAG_ISR : Interrupt status and enable */
#define ASPEED_JTAG_ISR_INST_PAUSE	BIT(19)
#define ASPEED_JTAG_ISR_INST_COMPLETE	BIT(18)
#define ASPEED_JTAG_ISR_DATA_PAUSE	BIT(17)
#define ASPEED_JTAG_ISR_DATA_COMPLETE	BIT(16)
#define ASPEED_JTAG_ISR_INST_PAUSE_EN	BIT(3)
#define ASPEED_JTAG_ISR_INST_COMPLETE_EN BIT(2)
#define ASPEED_JTAG_ISR_DATA_PAUSE_EN	BIT(1)
#define ASPEED_JTAG_ISR_DATA_COMPLETE_EN BIT(0)
#define ASPEED_JTAG_ISR_INT_EN_MASK	GENMASK(3, 0)
#define ASPEED_JTAG_ISR_INT_MASK	GENMASK(19, 16)

/* ASPEED_JTAG_SW : Software Mode and Status */
#define ASPEED_JTAG_SW_MODE_EN		BIT(19)
#define ASPEED_JTAG_SW_MODE_TCK		BIT(18)
#define ASPEED_JTAG_SW_MODE_TMS		BIT(17)
#define ASPEED_JTAG_SW_MODE_TDIO	BIT(16)

/* ASPEED_JTAG_TCK : TCK Control */
#define ASPEED_JTAG_TCK_DIVISOR_MASK	GENMASK(10, 0)
#define ASPEED_JTAG_TCK_GET_DIV(x)	((x) & ASPEED_JTAG_TCK_DIVISOR_MASK)

/* ASPEED_JTAG_EC : Controller set for go to IDLE */
#define ASPEED_JTAG_EC_GO_IDLE		BIT(0)

#define ASPEED_JTAG_IOUT_LEN(len) \
	(ASPEED_JTAG_CTL_ENG_EN | \
	 ASPEED_JTAG_CTL_ENG_OUT_EN | \
	 ASPEED_JTAG_CTL_INST_LEN(len))

#define ASPEED_JTAG_DOUT_LEN(len) \
	(ASPEED_JTAG_CTL_ENG_EN | \
	 ASPEED_JTAG_CTL_ENG_OUT_EN | \
	 ASPEED_JTAG_CTL_DATA_LEN(len))

#define ASPEED_JTAG_SW_TDIO (ASPEED_JTAG_SW_MODE_EN | ASPEED_JTAG_SW_MODE_TDIO)

#define ASPEED_JTAG_GET_TDI(direction, byte) \
	((direction == JTAG_READ_XFER) ? UINT_MAX : byte)

#define ASPEED_JTAG_TCK_WAIT		10
#define ASPEED_JTAG_RESET_CNTR		10

#define ASPEED_JTAG_NAME		"jtag-aspeed"

struct aspeed_jtag {
	void __iomem			*reg_base;
	struct device			*dev;
	struct clk			*pclk;
	enum jtag_endstate		status;
	int				irq;
	struct reset_control		*rst;
	u32				flag;
	wait_queue_head_t		jtag_wq;
	u32				mode;
};

static char *end_status_str[] = {"idle", "irpause", "drpause"};

static u32 aspeed_jtag_read(struct aspeed_jtag *aspeed_jtag, u32 reg)
{
	return readl(aspeed_jtag->reg_base + reg);
}

static void
aspeed_jtag_write(struct aspeed_jtag *aspeed_jtag, u32 val, u32 reg)
{
	writel(val, aspeed_jtag->reg_base + reg);
}

static int aspeed_jtag_freq_set(struct jtag *jtag, u32 freq)
{
	struct aspeed_jtag *aspeed_jtag = jtag_priv(jtag);
	unsigned long apb_frq;
	u32 tck_val;
	u16 div;

	apb_frq = clk_get_rate(aspeed_jtag->pclk);
	if (!apb_frq)
		return -ENOTSUPP;

	div = (apb_frq - 1) / freq;
	tck_val = aspeed_jtag_read(aspeed_jtag, ASPEED_JTAG_TCK);
	aspeed_jtag_write(aspeed_jtag,
			  (tck_val & ASPEED_JTAG_TCK_DIVISOR_MASK) | div,
			  ASPEED_JTAG_TCK);
	return 0;
}

static int aspeed_jtag_freq_get(struct jtag *jtag, u32 *frq)
{
	struct aspeed_jtag *aspeed_jtag = jtag_priv(jtag);
	u32 pclk;
	u32 tck;

	pclk = clk_get_rate(aspeed_jtag->pclk);
	tck = aspeed_jtag_read(aspeed_jtag, ASPEED_JTAG_TCK);
	*frq = pclk / (ASPEED_JTAG_TCK_GET_DIV(tck) + 1);

	return 0;
}

static int aspeed_jtag_mode_set(struct jtag *jtag, u32 mode)
{
	struct aspeed_jtag *aspeed_jtag = jtag_priv(jtag);

	aspeed_jtag->mode = mode;
	return 0;
}

static char aspeed_jtag_tck_cycle(struct aspeed_jtag *aspeed_jtag,
				  u8 tms, u8 tdi)
{
	char tdo = 0;

	/* TCK = 0 */
	aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_SW_MODE_EN |
			  (tms * ASPEED_JTAG_SW_MODE_TMS) |
			  (tdi * ASPEED_JTAG_SW_MODE_TDIO), ASPEED_JTAG_SW);

	ndelay(ASPEED_JTAG_TCK_WAIT);

	/* TCK = 1 */
	aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_SW_MODE_EN |
			  ASPEED_JTAG_SW_MODE_TCK |
			  (tms * ASPEED_JTAG_SW_MODE_TMS) |
			  (tdi * ASPEED_JTAG_SW_MODE_TDIO), ASPEED_JTAG_SW);

	if (aspeed_jtag_read(aspeed_jtag, ASPEED_JTAG_SW) &
	    ASPEED_JTAG_SW_MODE_TDIO)
		tdo = 1;

	ndelay(ASPEED_JTAG_TCK_WAIT);

	/* TCK = 0 */
	aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_SW_MODE_EN |
			  (tms * ASPEED_JTAG_SW_MODE_TMS) |
			  (tdi * ASPEED_JTAG_SW_MODE_TDIO), ASPEED_JTAG_SW);
	return tdo;
}

static void aspeed_jtag_wait_instruction_pause(struct aspeed_jtag *aspeed_jtag)
{
	wait_event_interruptible(aspeed_jtag->jtag_wq,
				 aspeed_jtag->flag &  ASPEED_JTAG_ISR_INST_PAUSE);
	aspeed_jtag->flag &= ~ASPEED_JTAG_ISR_INST_PAUSE;
}

static void
aspeed_jtag_wait_instruction_complete(struct aspeed_jtag *aspeed_jtag)
{
	wait_event_interruptible(aspeed_jtag->jtag_wq,
				 aspeed_jtag->flag & ASPEED_JTAG_ISR_INST_COMPLETE);
	aspeed_jtag->flag &= ~ASPEED_JTAG_ISR_INST_COMPLETE;
}

static void
aspeed_jtag_wait_data_pause_complete(struct aspeed_jtag *aspeed_jtag)
{
	wait_event_interruptible(aspeed_jtag->jtag_wq,
				 aspeed_jtag->flag & ASPEED_JTAG_ISR_DATA_PAUSE);
	aspeed_jtag->flag &= ~ASPEED_JTAG_ISR_DATA_PAUSE;
}

static void aspeed_jtag_wait_data_complete(struct aspeed_jtag *aspeed_jtag)
{
	wait_event_interruptible(aspeed_jtag->jtag_wq,
				 aspeed_jtag->flag & ASPEED_JTAG_ISR_DATA_COMPLETE);
	aspeed_jtag->flag &= ~ASPEED_JTAG_ISR_DATA_COMPLETE;
}

static void aspeed_jtag_sm_cycle(struct aspeed_jtag *aspeed_jtag,
				 const u8 *tms, int len)
{
	int i;

	for (i = 0; i < len; i++)
		aspeed_jtag_tck_cycle(aspeed_jtag, tms[i], 0);
}

static void aspeed_jtag_run_idle(struct aspeed_jtag *aspeed_jtag,
				 struct jtag_run_test_idle *runtest)
{
	static const u8 sm_idle_irpause[] = {1, 1, 0, 1, 0};
	static const u8 sm_idle_drpause[] = {1, 0, 1, 0};

	switch (runtest->endstate) {
	case JTAG_STATE_PAUSEIR:
		/* ->DRSCan->IRSCan->IRCap->IRExit1->PauseIR */
		aspeed_jtag_sm_cycle(aspeed_jtag, sm_idle_irpause,
				     sizeof(sm_idle_irpause));
		aspeed_jtag->status = JTAG_STATE_PAUSEIR;
		break;
	case JTAG_STATE_PAUSEDR:
		/* ->DRSCan->DRCap->DRExit1->PauseDR */
		aspeed_jtag_sm_cycle(aspeed_jtag, sm_idle_drpause,
				     sizeof(sm_idle_drpause));

		aspeed_jtag->status = JTAG_STATE_PAUSEDR;
		break;
	case JTAG_STATE_IDLE:
		/* IDLE */
		aspeed_jtag_tck_cycle(aspeed_jtag, 0, 0);
		aspeed_jtag->status = JTAG_STATE_IDLE;
		break;
	default:
		break;
	}
}

static void aspeed_jtag_run_pause(struct aspeed_jtag *aspeed_jtag,
				  struct jtag_run_test_idle *runtest)
{
	static const u8 sm_pause_irpause[] = {1, 1, 1, 1, 0, 1, 0};
	static const u8 sm_pause_drpause[] = {1, 1, 1, 0, 1, 0};
	static const u8 sm_pause_idle[] = {1, 1, 0};

	/* From IR/DR Pa.use */
	switch (runtest->endstate) {
	case JTAG_STATE_PAUSEIR:
		/*
		 * to Exit2 IR/DR->Updt IR/DR->DRSCan->IRSCan->IRCap->
		 * IRExit1->PauseIR
		 */
		aspeed_jtag_sm_cycle(aspeed_jtag, sm_pause_irpause,
				     sizeof(sm_pause_irpause));

		aspeed_jtag->status = JTAG_STATE_PAUSEIR;
		break;
	case JTAG_STATE_PAUSEDR:
		/*
		 * to Exit2 IR/DR->Updt IR/DR->DRSCan->DRCap->
		 * DRExit1->PauseDR
		 */
		aspeed_jtag_sm_cycle(aspeed_jtag, sm_pause_drpause,
				     sizeof(sm_pause_drpause));
		aspeed_jtag->status = JTAG_STATE_PAUSEDR;
		break;
	case JTAG_STATE_IDLE:
		/* to Exit2 IR/DR->Updt IR/DR->IDLE */
		aspeed_jtag_sm_cycle(aspeed_jtag, sm_pause_idle,
				     sizeof(sm_pause_idle));
		aspeed_jtag->status = JTAG_STATE_IDLE;
		break;
	default:
		break;
	}
}

static void aspeed_jtag_run_test_idle_sw(struct aspeed_jtag *aspeed_jtag,
					 struct jtag_run_test_idle *runtest)
{
	int i;

	/* SW mode from idle/pause-> to pause/idle */
	if (runtest->reset) {
		for (i = 0; i < ASPEED_JTAG_RESET_CNTR; i++)
			aspeed_jtag_tck_cycle(aspeed_jtag, 1, 0);
	}

	switch (aspeed_jtag->status) {
	case JTAG_STATE_IDLE:
		aspeed_jtag_run_idle(aspeed_jtag, runtest);
		break;

	case JTAG_STATE_PAUSEIR:
	/* Fall-through */
	case JTAG_STATE_PAUSEDR:
		aspeed_jtag_run_pause(aspeed_jtag, runtest);
		break;

	default:
		dev_err(aspeed_jtag->dev, "aspeed_jtag_run_test_idle error\n");
		break;
	}

	/* Stay on IDLE for at least  TCK cycle */
	for (i = 0; i < runtest->tck; i++)
		aspeed_jtag_tck_cycle(aspeed_jtag, 0, 0);
}

static int aspeed_jtag_idle(struct jtag *jtag,
			    struct jtag_run_test_idle *runtest)
{
	struct aspeed_jtag *aspeed_jtag = jtag_priv(jtag);

	dev_dbg(aspeed_jtag->dev, "runtest, state:%s\n",
		end_status_str[runtest->endstate]);

	if (!(aspeed_jtag->mode & JTAG_XFER_HW_MODE)) {
		aspeed_jtag_run_test_idle_sw(aspeed_jtag, runtest);
		return 0;
	}

	/* Disable sw mode */
	aspeed_jtag_write(aspeed_jtag, 0, ASPEED_JTAG_SW);
	/* x TMS high + 1 TMS low */
	if (runtest->reset)
		aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_CTL_ENG_EN |
				  ASPEED_JTAG_CTL_ENG_OUT_EN |
				  ASPEED_JTAG_CTL_FORCE_TMS, ASPEED_JTAG_CTRL);
	else
		aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_EC_GO_IDLE,
				  ASPEED_JTAG_EC);

	aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_SW_TDIO, ASPEED_JTAG_SW);

	aspeed_jtag->status = JTAG_STATE_IDLE;
	return 0;
}

static void aspeed_jtag_xfer_sw(struct aspeed_jtag *aspeed_jtag,
				struct jtag_xfer *xfer, u32 *data)
{
	static const u8 sm_update_shiftir[] = { 1, 1, 0, 0 };
	static const u8 sm_update_shiftdr[] = { 1, 0, 0 };
	static const u8 sm_pause_idle[] = { 1, 1, 0 };
	static const u8 sm_pause_update[] = { 1, 1 };
	unsigned long remain_xfer = xfer->length;
	unsigned long shift_bits = 0;
	unsigned long index = 0;
	unsigned long tdi;
	char tdo;

	if (aspeed_jtag->status != JTAG_STATE_IDLE) {
		/*IR/DR Pause->Exit2 IR / DR->Update IR /DR */
		aspeed_jtag_sm_cycle(aspeed_jtag, sm_pause_update,
				     sizeof(sm_pause_update));
	}

	if (xfer->type == JTAG_SIR_XFER)
		/* ->IRSCan->CapIR->ShiftIR */
		aspeed_jtag_sm_cycle(aspeed_jtag, sm_update_shiftir,
				     sizeof(sm_update_shiftir));
	else
		/* ->DRScan->DRCap->DRShift */
		aspeed_jtag_sm_cycle(aspeed_jtag, sm_update_shiftdr,
				     sizeof(sm_update_shiftdr));

	tdi = ASPEED_JTAG_GET_TDI(xfer->direction, data[index]);

	while (remain_xfer > 1) {
		tdo = aspeed_jtag_tck_cycle(aspeed_jtag, 0,
					    tdi & ASPEED_JTAG_DATA_MSB);
		data[index] |= tdo << (shift_bits %
					    ASPEED_JTAG_DATA_CHUNK_SIZE);

		tdi >>= 1;
		shift_bits++;
		remain_xfer--;

		if (shift_bits % ASPEED_JTAG_DATA_CHUNK_SIZE == 0) {
			tdo = 0;
			index++;

			tdi = ASPEED_JTAG_GET_TDI(xfer->direction, data[index]);
		}
	}

	tdo = aspeed_jtag_tck_cycle(aspeed_jtag, 1, tdi & ASPEED_JTAG_DATA_MSB);
	data[index] |= tdo << (shift_bits % ASPEED_JTAG_DATA_CHUNK_SIZE);

	/* DIPause/DRPause */
	aspeed_jtag_tck_cycle(aspeed_jtag, 0, 0);

	if (xfer->endstate == JTAG_STATE_IDLE) {
		/* ->DRExit2->DRUpdate->IDLE */
		aspeed_jtag_sm_cycle(aspeed_jtag, sm_pause_idle,
				     sizeof(sm_pause_idle));
	}
}

static void aspeed_jtag_xfer_push_data(struct aspeed_jtag *aspeed_jtag,
				       enum jtag_xfer_type type, u32 bits_len)
{
	if (type == JTAG_SIR_XFER) {
		aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_IOUT_LEN(bits_len),
				  ASPEED_JTAG_CTRL);
		aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_DOUT_LEN(bits_len) |
				  ASPEED_JTAG_CTL_INST_EN, ASPEED_JTAG_CTRL);
	} else {
		aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_DOUT_LEN(bits_len),
				  ASPEED_JTAG_CTRL);
		aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_DOUT_LEN(bits_len) |
				  ASPEED_JTAG_CTL_DATA_EN, ASPEED_JTAG_CTRL);
	}
}

static void aspeed_jtag_xfer_push_data_last(struct aspeed_jtag *aspeed_jtag,
					    enum jtag_xfer_type type,
					    u32 shift_bits,
					    enum jtag_endstate endstate)
{
	if (endstate == JTAG_STATE_IDLE) {
		if (type == JTAG_SIR_XFER) {
			aspeed_jtag_write(aspeed_jtag,
					  ASPEED_JTAG_IOUT_LEN(shift_bits),
					  ASPEED_JTAG_CTRL);
			aspeed_jtag_write(aspeed_jtag,
					  ASPEED_JTAG_IOUT_LEN(shift_bits) |
					  ASPEED_JTAG_CTL_INST_EN,
					  ASPEED_JTAG_CTRL);
			aspeed_jtag_wait_instruction_pause(aspeed_jtag);
		} else {
			aspeed_jtag_write(aspeed_jtag,
					  ASPEED_JTAG_DOUT_LEN(shift_bits) |
					  ASPEED_JTAG_CTL_DR_UPDATE,
					  ASPEED_JTAG_CTRL);
			aspeed_jtag_write(aspeed_jtag,
					  ASPEED_JTAG_DOUT_LEN(shift_bits) |
					  ASPEED_JTAG_CTL_DR_UPDATE |
					  ASPEED_JTAG_CTL_DATA_EN,
					  ASPEED_JTAG_CTRL);
			aspeed_jtag_wait_data_pause_complete(aspeed_jtag);
		}
	} else {
		if (type == JTAG_SIR_XFER) {
			aspeed_jtag_write(aspeed_jtag,
					  ASPEED_JTAG_IOUT_LEN(shift_bits) |
					  ASPEED_JTAG_CTL_LASPEED_INST,
					  ASPEED_JTAG_CTRL);
			aspeed_jtag_write(aspeed_jtag,
					  ASPEED_JTAG_IOUT_LEN(shift_bits) |
					  ASPEED_JTAG_CTL_LASPEED_INST |
					  ASPEED_JTAG_CTL_INST_EN,
					  ASPEED_JTAG_CTRL);
			aspeed_jtag_wait_instruction_complete(aspeed_jtag);
		} else {
			aspeed_jtag_write(aspeed_jtag,
					  ASPEED_JTAG_DOUT_LEN(shift_bits) |
					  ASPEED_JTAG_CTL_LASPEED_DATA,
					  ASPEED_JTAG_CTRL);
			aspeed_jtag_write(aspeed_jtag,
					  ASPEED_JTAG_DOUT_LEN(shift_bits) |
					  ASPEED_JTAG_CTL_LASPEED_DATA |
					  ASPEED_JTAG_CTL_DATA_EN,
					  ASPEED_JTAG_CTRL);
			aspeed_jtag_wait_data_complete(aspeed_jtag);
		}
	}
}

static void aspeed_jtag_xfer_hw(struct aspeed_jtag *aspeed_jtag,
				struct jtag_xfer *xfer, u32 *data)
{
	unsigned long remain_xfer = xfer->length;
	unsigned long index = 0;
	char shift_bits;
	u32 data_reg;

	data_reg = xfer->type == JTAG_SIR_XFER ?
		   ASPEED_JTAG_INST : ASPEED_JTAG_DATA;
	while (remain_xfer) {
		if (xfer->direction == JTAG_WRITE_XFER)
			aspeed_jtag_write(aspeed_jtag, data[index], data_reg);
		else
			aspeed_jtag_write(aspeed_jtag, 0, data_reg);

		if (remain_xfer > ASPEED_JTAG_DATA_CHUNK_SIZE) {
			shift_bits = ASPEED_JTAG_DATA_CHUNK_SIZE;

			/*
			 * Read bytes were not equals to column length
			 * and go to Pause-DR
			 */
			aspeed_jtag_xfer_push_data(aspeed_jtag, xfer->type,
						   shift_bits);
		} else {
			/*
			 * Read bytes equals to column length =>
			 * Update-DR
			 */
			shift_bits = remain_xfer;
			aspeed_jtag_xfer_push_data_last(aspeed_jtag, xfer->type,
							shift_bits,
							xfer->endstate);
		}

		if (xfer->direction == JTAG_READ_XFER) {
			if (shift_bits < ASPEED_JTAG_DATA_CHUNK_SIZE) {
				data[index] = aspeed_jtag_read(aspeed_jtag,
							       data_reg);

				data[index] >>= ASPEED_JTAG_DATA_CHUNK_SIZE -
								shift_bits;
			} else {
				data[index] = aspeed_jtag_read(aspeed_jtag,
							       data_reg);
			}
		}

		remain_xfer = remain_xfer - shift_bits;
		index++;
	}
}

static int aspeed_jtag_xfer(struct jtag *jtag, struct jtag_xfer *xfer,
			    u8 *xfer_data)
{
	struct aspeed_jtag *aspeed_jtag = jtag_priv(jtag);

	dev_dbg(aspeed_jtag->dev, "xfer %s\n",
		xfer->type == JTAG_SIR_XFER ? "SIR" : "SDR");

	if (!(aspeed_jtag->mode & JTAG_XFER_HW_MODE)) {
		/* SW mode */
		aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_SW_TDIO, ASPEED_JTAG_SW);

		aspeed_jtag_xfer_sw(aspeed_jtag, xfer, (u32 *)xfer_data);
	} else {
		/* HW mode */
		aspeed_jtag_write(aspeed_jtag, 0, ASPEED_JTAG_SW);
		aspeed_jtag_xfer_hw(aspeed_jtag, xfer, (u32 *)xfer_data);
	}

	aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_SW_TDIO, ASPEED_JTAG_SW);
	aspeed_jtag->status = xfer->endstate;
	return 0;
}

static int aspeed_jtag_status_get(struct jtag *jtag, u32 *status)
{
	struct aspeed_jtag *aspeed_jtag = jtag_priv(jtag);

	*status = aspeed_jtag->status;
	return 0;
}

static irqreturn_t aspeed_jtag_interrupt(s32 this_irq, void *dev_id)
{
	struct aspeed_jtag *aspeed_jtag = dev_id;
	irqreturn_t ret;
	u32 status;

	status = aspeed_jtag_read(aspeed_jtag, ASPEED_JTAG_ISR);

	if (status & ASPEED_JTAG_ISR_INT_MASK) {
		aspeed_jtag_write(aspeed_jtag,
				  (status & ASPEED_JTAG_ISR_INT_MASK)
				  | (status & ASPEED_JTAG_ISR_INT_EN_MASK),
				  ASPEED_JTAG_ISR);
		aspeed_jtag->flag |= status & ASPEED_JTAG_ISR_INT_MASK;
	}

	if (aspeed_jtag->flag) {
		wake_up_interruptible(&aspeed_jtag->jtag_wq);
		ret = IRQ_HANDLED;
	} else {
		dev_err(aspeed_jtag->dev, "irq status:%x\n",
			status);
		ret = IRQ_NONE;
	}
	return ret;
}

static int aspeed_jtag_init(struct platform_device *pdev,
			    struct aspeed_jtag *aspeed_jtag)
{
	struct resource *res;
	int err;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	aspeed_jtag->reg_base = devm_ioremap_resource(aspeed_jtag->dev, res);
	if (IS_ERR(aspeed_jtag->reg_base))
		return -ENOMEM;

	aspeed_jtag->pclk = devm_clk_get(aspeed_jtag->dev, NULL);
	if (IS_ERR(aspeed_jtag->pclk)) {
		dev_err(aspeed_jtag->dev, "devm_clk_get failed\n");
		return PTR_ERR(aspeed_jtag->pclk);
	}

	aspeed_jtag->irq = platform_get_irq(pdev, 0);
	if (aspeed_jtag->irq < 0) {
		dev_err(aspeed_jtag->dev, "no irq specified\n");
		return -ENOENT;
	}

	if (clk_prepare_enable(aspeed_jtag->pclk)) {
		dev_err(aspeed_jtag->dev, "no irq specified\n");
		return -ENOENT;
	}

	aspeed_jtag->rst = devm_reset_control_get_shared(&pdev->dev, NULL);
	if (IS_ERR(aspeed_jtag->rst)) {
		dev_err(aspeed_jtag->dev,
			"missing or invalid reset controller device tree entry");
		return PTR_ERR(aspeed_jtag->rst);
	}
	reset_control_deassert(aspeed_jtag->rst);

	/* Enable clock */
	aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_CTL_ENG_EN |
			  ASPEED_JTAG_CTL_ENG_OUT_EN, ASPEED_JTAG_CTRL);
	aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_SW_TDIO, ASPEED_JTAG_SW);

	err = devm_request_irq(aspeed_jtag->dev, aspeed_jtag->irq,
			       aspeed_jtag_interrupt, 0,
			       "aspeed-jtag", aspeed_jtag);
	if (err) {
		dev_err(aspeed_jtag->dev, "unable to get IRQ");
		goto clk_unprep;
	}

	aspeed_jtag_write(aspeed_jtag, ASPEED_JTAG_ISR_INST_PAUSE |
			  ASPEED_JTAG_ISR_INST_COMPLETE |
			  ASPEED_JTAG_ISR_DATA_PAUSE |
			  ASPEED_JTAG_ISR_DATA_COMPLETE |
			  ASPEED_JTAG_ISR_INST_PAUSE_EN |
			  ASPEED_JTAG_ISR_INST_COMPLETE_EN |
			  ASPEED_JTAG_ISR_DATA_PAUSE_EN |
			  ASPEED_JTAG_ISR_DATA_COMPLETE_EN,
			  ASPEED_JTAG_ISR);

	aspeed_jtag->flag = 0;
	aspeed_jtag->mode = 0;
	init_waitqueue_head(&aspeed_jtag->jtag_wq);
	return 0;

clk_unprep:
	clk_disable_unprepare(aspeed_jtag->pclk);
	return err;
}

static int aspeed_jtag_deinit(struct platform_device *pdev,
			      struct aspeed_jtag *aspeed_jtag)
{
	aspeed_jtag_write(aspeed_jtag, 0, ASPEED_JTAG_ISR);
	/* Disable clock */
	aspeed_jtag_write(aspeed_jtag, 0, ASPEED_JTAG_CTRL);
	reset_control_assert(aspeed_jtag->rst);
	clk_disable_unprepare(aspeed_jtag->pclk);
	return 0;
}

static const struct jtag_ops aspeed_jtag_ops = {
	.freq_get = aspeed_jtag_freq_get,
	.freq_set = aspeed_jtag_freq_set,
	.status_get = aspeed_jtag_status_get,
	.idle = aspeed_jtag_idle,
	.xfer = aspeed_jtag_xfer,
	.mode_set = aspeed_jtag_mode_set
};

static int aspeed_jtag_probe(struct platform_device *pdev)
{
	struct aspeed_jtag *aspeed_jtag;
	struct jtag *jtag;
	int err;

	jtag = jtag_alloc(&pdev->dev, sizeof(*aspeed_jtag), &aspeed_jtag_ops);
	if (!jtag)
		return -ENOMEM;

	platform_set_drvdata(pdev, jtag);
	aspeed_jtag = jtag_priv(jtag);
	aspeed_jtag->dev = &pdev->dev;

	/* Initialize device*/
	err = aspeed_jtag_init(pdev, aspeed_jtag);
	if (err)
		goto err_jtag_init;

	/* Initialize JTAG core structure*/
	err = devm_jtag_register(aspeed_jtag->dev, jtag);
	if (err)
		goto err_jtag_register;

	return 0;

err_jtag_register:
	aspeed_jtag_deinit(pdev, aspeed_jtag);
err_jtag_init:
	jtag_free(jtag);
	return err;
}

static int aspeed_jtag_remove(struct platform_device *pdev)
{
	struct jtag *jtag = platform_get_drvdata(pdev);

	aspeed_jtag_deinit(pdev, jtag_priv(jtag));
	return 0;
}

static const struct of_device_id aspeed_jtag_of_match[] = {
	{ .compatible = "aspeed,ast2400-jtag", },
	{ .compatible = "aspeed,ast2500-jtag", },
	{}
};

static struct platform_driver aspeed_jtag_driver = {
	.probe = aspeed_jtag_probe,
	.remove = aspeed_jtag_remove,
	.driver = {
		.name = ASPEED_JTAG_NAME,
		.of_match_table = aspeed_jtag_of_match,
	},
};
module_platform_driver(aspeed_jtag_driver);

MODULE_AUTHOR("Oleksandr Shamray <oleksandrs@mellanox.com>");
MODULE_DESCRIPTION("ASPEED JTAG driver");
MODULE_LICENSE("GPL v2");
