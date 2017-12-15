// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2015-2017, Intel Corporation.

#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/kcs-bmc.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/regmap.h>
#include <linux/sched.h>
#include <linux/timer.h>

#define KCS_MSG_BUFSIZ      1024
#define KCS_CHANNEL_MAX     4

/*
 * This is a BMC device used to communicate to the host
 */
#define DEVICE_NAME     "ipmi-kcs-host"


/* Different Phases of the KCS Module */
#define KCS_PHASE_IDLE          0x00
#define KCS_PHASE_WRITE         0x01
#define KCS_PHASE_WRITE_END     0x02
#define KCS_PHASE_READ          0x03
#define KCS_PHASE_ABORT         0x04
#define KCS_PHASE_ERROR         0x05

/* Abort Phase */
#define ABORT_PHASE_ERROR1      0x01
#define ABORT_PHASE_ERROR2      0x02

/* KCS Command Control codes. */
#define KCS_GET_STATUS          0x60
#define KCS_ABORT               0x60
#define KCS_WRITE_START         0x61
#define KCS_WRITE_END           0x62
#define KCS_READ_BYTE           0x68

/* Status bits.:
 * - IDLE_STATE.  Interface is idle. System software should not be expecting
 *                nor sending any data.
 * - READ_STATE.  BMC is transferring a packet to system software. System
 *                software should be in the "Read Message" state.
 * - WRITE_STATE. BMC is receiving a packet from system software. System
 *                software should be writing a command to the BMC.
 * - ERROR_STATE. BMC has detected a protocol violation at the interface level,
 *                or the transfer has been aborted. System software can either
 *                use the "Get_Status" control code to request the nature of
 *                the error, or it can just retry the command.
 */
#define KCS_IDLE_STATE           0
#define KCS_READ_STATE           1
#define KCS_WRITE_STATE          2
#define KCS_ERROR_STATE          3

/* KCS Error Codes */
#define KCS_NO_ERROR                0x00
#define KCS_ABORTED_BY_COMMAND      0x01
#define KCS_ILLEGAL_CONTROL_CODE    0x02
#define KCS_LENGTH_ERROR            0x06
#define KCS_UNSPECIFIED_ERROR       0xFF


#define KCS_ZERO_DATA           0

/* IPMI 2.0 - Table 9-1, KCS Interface Status Register Bits */
#define KCS_STR_STATE(state)        (state << 6)
#define KCS_STR_STATE_MASK          KCS_STR_STATE(0x3)
#define KCS_STR_CMD_DAT             BIT(3)
#define KCS_STR_ATN                 BIT(2)
#define KCS_STR_IBF                 BIT(1)
#define KCS_STR_OBF                 BIT(0)


/***************************** LPC Register ****************************/
/* mapped to lpc-bmc@0 IO space */
#define LPC_HICR0            0x000
#define     LPC_HICR0_LPC3E          BIT(7)
#define     LPC_HICR0_LPC2E          BIT(6)
#define     LPC_HICR0_LPC1E          BIT(5)
#define LPC_HICR2            0x008
#define     LPC_HICR2_IBFIF3         BIT(3)
#define     LPC_HICR2_IBFIF2         BIT(2)
#define     LPC_HICR2_IBFIF1         BIT(1)
#define LPC_HICR4            0x010
#define     LPC_HICR4_LADR12AS       BIT(7)
#define     LPC_HICR4_KCSENBL        BIT(2)
#define LPC_LADR3H           0x014
#define LPC_LADR3L           0x018
#define LPC_LADR12H          0x01C
#define LPC_LADR12L          0x020
#define LPC_IDR1             0x024
#define LPC_IDR2             0x028
#define LPC_IDR3             0x02C
#define LPC_ODR1             0x030
#define LPC_ODR2             0x034
#define LPC_ODR3             0x038
#define LPC_STR1             0x03C
#define LPC_STR2             0x040
#define LPC_STR3             0x044

/* mapped to lpc-host@80 IO space */
#define LPC_HICRB            0x080
#define     LPC_HICRB_IBFIF4         BIT(1)
#define     LPC_HICRB_LPC4E          BIT(0)
#define LPC_LADR4            0x090
#define LPC_IDR4             0x094
#define LPC_ODR4             0x098
#define LPC_STR4             0x09C


/* IPMI 2.0 - 9.5, KCS Interface Registers */
struct kcs_ioreg {
	u32 idr; /* Input Data Register */
	u32 odr; /* Output Data Register */
	u32 str; /* Status Register */
};

static const struct kcs_ioreg kcs_channel_ioregs[KCS_CHANNEL_MAX] = {
	{ .idr = LPC_IDR1, .odr = LPC_ODR1, .str = LPC_STR1 },
	{ .idr = LPC_IDR2, .odr = LPC_ODR2, .str = LPC_STR2 },
	{ .idr = LPC_IDR3, .odr = LPC_ODR3, .str = LPC_STR3 },
	{ .idr = LPC_IDR4, .odr = LPC_ODR4, .str = LPC_STR4 },
};

struct kcs_bmc {
	struct regmap *map;
	int            irq;
	spinlock_t     lock;

	u32 chan;
	int running;

	u32 idr;
	u32 odr;
	u32 str;

	int kcs_phase;
	u8  abort_phase;
	u8  kcs_error;

	wait_queue_head_t queue;
	int  data_in_avail;
	int  data_in_idx;
	u8  *data_in;

	int  data_out_idx;
	int  data_out_len;
	u8  *data_out;

	struct miscdevice miscdev;
	char name[16];
};

static u8 kcs_inb(struct kcs_bmc *kcs_bmc, u32 reg)
{
	u32 val = 0;
	int rc;

	rc = regmap_read(kcs_bmc->map, reg, &val);
	WARN(rc != 0, "kcs_inb failed: %d\n", rc);

	return rc == 0 ? (u8) val : 0;
}

static void kcs_outb(struct kcs_bmc *kcs_bmc, u8 data, u32 reg)
{
	int rc;

	rc = regmap_write(kcs_bmc->map, reg, data);
	WARN(rc != 0, "kcs_outb failed: %d\n", rc);
}

static void kcs_set_state(struct kcs_bmc *kcs_bmc, u8 state)
{
	int rc;

	rc = regmap_update_bits(kcs_bmc->map, kcs_bmc->str,
				KCS_STR_STATE_MASK,
				KCS_STR_STATE(state));
	WARN(rc != 0, "KCS_STR_STATE failed: %d\n", rc);
}

static void kcs_set_atn(struct kcs_bmc *kcs_bmc, unsigned long set)
{
	int rc;

	rc = regmap_update_bits(kcs_bmc->map, kcs_bmc->str,
				KCS_STR_ATN,
				set != 0 ? KCS_STR_ATN : 0);
	WARN(rc != 0, "KCS_STR_ATN failed: %d\n", rc);
}

/*********************************************************************
 * AST_usrGuide_KCS.pdf
 * 2. Background:
 *   we note D for Data, and C for Cmd/Status, default rules are
 *     A. KCS1 / KCS2 ( D / C:X / X+4 )
 *        D / C : CA0h / CA4h
 *        D / C : CA8h / CACh
 *     B. KCS3 ( D / C:XX2h / XX3h )
 *        D / C : CA2h / CA3h
 *        D / C : CB2h / CB3h
 *     C. KCS4
 *        D / C : CA4h / CA5h
 *********************************************************************/
void kcs_set_addr(struct kcs_bmc *kcs_bmc, u16 addr)
{
	switch (kcs_bmc->chan) {
	case 1:
		regmap_update_bits(kcs_bmc->map, LPC_HICR4,
				LPC_HICR4_LADR12AS,
				0);
		regmap_write(kcs_bmc->map, LPC_LADR12H, addr >> 8);
		regmap_write(kcs_bmc->map, LPC_LADR12L, addr & 0xFF);
		break;

	case 2:
		regmap_update_bits(kcs_bmc->map, LPC_HICR4,
				LPC_HICR4_LADR12AS,
				LPC_HICR4_LADR12AS);
		regmap_write(kcs_bmc->map, LPC_LADR12H, addr >> 8);
		regmap_write(kcs_bmc->map, LPC_LADR12L, addr & 0xFF);
		break;

	case 3:
		regmap_write(kcs_bmc->map, LPC_LADR3H, addr >> 8);
		regmap_write(kcs_bmc->map, LPC_LADR3L, addr & 0xFF);
		break;

	case 4:
		regmap_write(kcs_bmc->map, LPC_LADR4,
				((addr + 1) << 16) | (addr));
		break;

	default:
		break;
	}
}

static void kcs_enable_channel(struct kcs_bmc *kcs_bmc, int enable)
{
	switch (kcs_bmc->chan) {
	case 1:
		if (enable) {
			regmap_update_bits(kcs_bmc->map, LPC_HICR2,
					LPC_HICR2_IBFIF1, LPC_HICR2_IBFIF1);
			regmap_update_bits(kcs_bmc->map, LPC_HICR0,
					LPC_HICR0_LPC1E, LPC_HICR0_LPC1E);
		} else {
			regmap_update_bits(kcs_bmc->map, LPC_HICR0,
					LPC_HICR0_LPC1E, 0);
			regmap_update_bits(kcs_bmc->map, LPC_HICR2,
					LPC_HICR2_IBFIF1, 0);
		}
		break;

	case 2:
		if (enable) {
			regmap_update_bits(kcs_bmc->map, LPC_HICR2,
					LPC_HICR2_IBFIF2, LPC_HICR2_IBFIF2);
			regmap_update_bits(kcs_bmc->map, LPC_HICR0,
					LPC_HICR0_LPC2E, LPC_HICR0_LPC2E);
		} else {
			regmap_update_bits(kcs_bmc->map, LPC_HICR0,
					LPC_HICR0_LPC2E, 0);
			regmap_update_bits(kcs_bmc->map, LPC_HICR2,
					LPC_HICR2_IBFIF2, 0);
		}
		break;

	case 3:
		if (enable) {
			regmap_update_bits(kcs_bmc->map, LPC_HICR2,
					LPC_HICR2_IBFIF3, LPC_HICR2_IBFIF3);
			regmap_update_bits(kcs_bmc->map, LPC_HICR0,
					LPC_HICR0_LPC3E, LPC_HICR0_LPC3E);
			regmap_update_bits(kcs_bmc->map, LPC_HICR4,
					LPC_HICR4_KCSENBL, LPC_HICR4_KCSENBL);
		} else {
			regmap_update_bits(kcs_bmc->map, LPC_HICR0,
					LPC_HICR0_LPC3E, 0);
			regmap_update_bits(kcs_bmc->map, LPC_HICR4,
					LPC_HICR4_KCSENBL, 0);
			regmap_update_bits(kcs_bmc->map, LPC_HICR2,
					LPC_HICR2_IBFIF3, 0);
		}
		break;

	case 4:
		if (enable) {
			regmap_update_bits(kcs_bmc->map, LPC_HICRB,
					LPC_HICRB_IBFIF4 | LPC_HICRB_LPC4E,
					LPC_HICRB_IBFIF4 | LPC_HICRB_LPC4E);
		} else {
			regmap_update_bits(kcs_bmc->map, LPC_HICRB,
					LPC_HICRB_IBFIF4 | LPC_HICRB_LPC4E,
					0);
		}
		break;

	default:
		break;
	}
}

static void kcs_rx_data(struct kcs_bmc *kcs_bmc)
{
	u8 data;

	switch (kcs_bmc->kcs_phase) {
	case KCS_PHASE_WRITE:
		kcs_set_state(kcs_bmc, KCS_WRITE_STATE);

		/* set OBF before reading data */
		kcs_outb(kcs_bmc, KCS_ZERO_DATA, kcs_bmc->odr);

		if (kcs_bmc->data_in_idx < KCS_MSG_BUFSIZ)
			kcs_bmc->data_in[kcs_bmc->data_in_idx++] =
					kcs_inb(kcs_bmc, kcs_bmc->idr);
		break;

	case KCS_PHASE_WRITE_END:
		kcs_set_state(kcs_bmc, KCS_READ_STATE);

		if (kcs_bmc->data_in_idx < KCS_MSG_BUFSIZ)
			kcs_bmc->data_in[kcs_bmc->data_in_idx++] =
					kcs_inb(kcs_bmc, kcs_bmc->idr);

		kcs_bmc->kcs_phase = KCS_PHASE_READ;
		if (kcs_bmc->running) {
			kcs_bmc->data_in_avail = 1;
			wake_up_interruptible(&kcs_bmc->queue);
		}
		break;

	case KCS_PHASE_READ:
		if (kcs_bmc->data_out_idx == kcs_bmc->data_out_len)
			kcs_set_state(kcs_bmc, KCS_IDLE_STATE);

		data = kcs_inb(kcs_bmc, kcs_bmc->idr);
		if (data != KCS_READ_BYTE) {
			kcs_set_state(kcs_bmc, KCS_ERROR_STATE);
			kcs_outb(kcs_bmc, KCS_ZERO_DATA, kcs_bmc->odr);
			break;
		}

		if (kcs_bmc->data_out_idx == kcs_bmc->data_out_len) {
			kcs_outb(kcs_bmc, KCS_ZERO_DATA, kcs_bmc->odr);
			kcs_bmc->kcs_phase = KCS_PHASE_IDLE;
			break;
		}

		kcs_outb(kcs_bmc, kcs_bmc->data_out[kcs_bmc->data_out_idx++],
				 kcs_bmc->odr);
		break;

	case KCS_PHASE_ABORT:
		switch (kcs_bmc->abort_phase) {
		case ABORT_PHASE_ERROR1:
			kcs_set_state(kcs_bmc, KCS_READ_STATE);

			/* Read the Dummy byte */
			kcs_inb(kcs_bmc, kcs_bmc->idr);

			kcs_outb(kcs_bmc, kcs_bmc->kcs_error, kcs_bmc->odr);
			kcs_bmc->abort_phase = ABORT_PHASE_ERROR2;
			break;

		case ABORT_PHASE_ERROR2:
			kcs_set_state(kcs_bmc, KCS_IDLE_STATE);

			/* Read the Dummy byte */
			kcs_inb(kcs_bmc, kcs_bmc->idr);

			kcs_outb(kcs_bmc, KCS_ZERO_DATA, kcs_bmc->odr);
			kcs_bmc->kcs_phase = KCS_PHASE_IDLE;
			kcs_bmc->abort_phase = 0;
			break;

		default:
			break;
		}

		break;

	case KCS_PHASE_ERROR:
		kcs_set_state(kcs_bmc, KCS_ERROR_STATE);

		/* Read the Dummy byte */
		kcs_inb(kcs_bmc, kcs_bmc->idr);

		kcs_outb(kcs_bmc, KCS_ZERO_DATA, kcs_bmc->odr);
		break;

	default:
		kcs_set_state(kcs_bmc, KCS_ERROR_STATE);

		/* Read the Dummy byte */
		kcs_inb(kcs_bmc, kcs_bmc->idr);

		kcs_outb(kcs_bmc, KCS_ZERO_DATA, kcs_bmc->odr);
		break;
	}
}

static void kcs_rx_cmd(struct kcs_bmc *kcs_bmc)
{
	u8 cmd;

	kcs_set_state(kcs_bmc, KCS_WRITE_STATE);

	/* Dummy data to generate OBF */
	kcs_outb(kcs_bmc, KCS_ZERO_DATA, kcs_bmc->odr);

	cmd = kcs_inb(kcs_bmc, kcs_bmc->idr);
	switch (cmd) {
	case KCS_WRITE_START:
		kcs_bmc->data_in_avail = 0;
		kcs_bmc->data_in_idx   = 0;
		kcs_bmc->kcs_phase     = KCS_PHASE_WRITE;
		kcs_bmc->kcs_error     = KCS_NO_ERROR;
		break;

	case KCS_WRITE_END:
		kcs_bmc->kcs_phase = KCS_PHASE_WRITE_END;
		break;

	case KCS_ABORT:
		if (kcs_bmc->kcs_error == KCS_NO_ERROR)
			kcs_bmc->kcs_error = KCS_ABORTED_BY_COMMAND;

		kcs_bmc->kcs_phase   = KCS_PHASE_ABORT;
		kcs_bmc->abort_phase = ABORT_PHASE_ERROR1;
		break;

	default:
		kcs_bmc->kcs_error = KCS_ILLEGAL_CONTROL_CODE;
		kcs_set_state(kcs_bmc, KCS_ERROR_STATE);
		kcs_outb(kcs_bmc, kcs_bmc->kcs_error, kcs_bmc->odr);
		kcs_bmc->kcs_phase = KCS_PHASE_ERROR;
		break;
	}
}

/*
 * Whenever the BMC is reset (from power-on or a hard reset), the State Bits
 * are initialized to "11 - Error State". Doing so allows SMS to detect that
 * the BMC has been reset and that any message in process has been terminated
 * by the BMC.
 */
static void kcs_force_abort(struct kcs_bmc *kcs_bmc)
{
	unsigned long flags;

	spin_lock_irqsave(&kcs_bmc->lock, flags);
	kcs_set_state(kcs_bmc, KCS_ERROR_STATE);

	/* Read the Dummy byte */
	kcs_inb(kcs_bmc, kcs_bmc->idr);

	kcs_outb(kcs_bmc, KCS_ZERO_DATA, kcs_bmc->odr);
	kcs_bmc->kcs_phase = KCS_PHASE_ERROR;
	spin_unlock_irqrestore(&kcs_bmc->lock, flags);
}

static irqreturn_t kcs_bmc_irq(int irq, void *arg)
{
	int rc;
	u32 sts;
	struct kcs_bmc *kcs_bmc = arg;

	rc = regmap_read(kcs_bmc->map, kcs_bmc->str, &sts);
	if (rc)
		return IRQ_NONE;

	sts &= (KCS_STR_IBF | KCS_STR_CMD_DAT);

	switch (sts) {
	case KCS_STR_IBF | KCS_STR_CMD_DAT:
		kcs_rx_cmd(kcs_bmc);
		break;

	case KCS_STR_IBF:
		kcs_rx_data(kcs_bmc);

	default:
		return IRQ_NONE;
	}

	return IRQ_HANDLED;
}

static int kcs_bmc_config_irq(struct kcs_bmc *kcs_bmc,
			struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int rc;

	kcs_bmc->irq = platform_get_irq(pdev, 0);
	if (!kcs_bmc->irq)
		return -ENODEV;

	rc = devm_request_irq(dev, kcs_bmc->irq, kcs_bmc_irq, IRQF_SHARED,
			kcs_bmc->name, kcs_bmc);
	if (rc < 0) {
		dev_warn(dev, "Unable to request IRQ %d\n", kcs_bmc->irq);
		kcs_bmc->irq = 0;
		return rc;
	}

	return rc;
}


static inline struct kcs_bmc *file_kcs_bmc(struct file *filp)
{
	return container_of(filp->private_data, struct kcs_bmc, miscdev);
}

static int kcs_bmc_open(struct inode *inode, struct file *filp)
{
	unsigned long flags;
	struct kcs_bmc *kcs_bmc = file_kcs_bmc(filp);

	if (kcs_bmc->running)
		return -EBUSY;

	spin_lock_irqsave(&kcs_bmc->lock, flags);
	kcs_bmc->kcs_phase     = KCS_PHASE_IDLE;
	kcs_bmc->running       = 1;
	kcs_bmc->data_in_avail = 0;
	spin_unlock_irqrestore(&kcs_bmc->lock, flags);

	return 0;
}

static unsigned int kcs_bmc_poll(struct file *filp, poll_table *wait)
{
	unsigned int mask = 0;
	struct kcs_bmc *kcs_bmc = file_kcs_bmc(filp);

	poll_wait(filp, &kcs_bmc->queue, wait);

	if (kcs_bmc->data_in_avail)
		mask |= POLLIN;

	if (kcs_bmc->kcs_phase == KCS_PHASE_READ)
		mask |= POLLOUT;

	return mask;
}

static ssize_t kcs_bmc_read(struct file *filp, char *buf,
			    size_t count, loff_t *offset)
{
	int rv;
	struct kcs_bmc *kcs_bmc = file_kcs_bmc(filp);

	rv = wait_event_interruptible(kcs_bmc->queue,
				kcs_bmc->data_in_avail != 0);
	if (rv < 0)
		return -ERESTARTSYS;

	kcs_bmc->data_in_avail = 0;

	if (count > kcs_bmc->data_in_idx)
		count = kcs_bmc->data_in_idx;

	if (copy_to_user(buf, kcs_bmc->data_in, count))
		return -EFAULT;

	return count;
}

static ssize_t kcs_bmc_write(struct file *filp, const char *buf,
			     size_t count, loff_t *offset)
{
	unsigned long flags;
	struct kcs_bmc *kcs_bmc = file_kcs_bmc(filp);

	if (count < 1 || count > KCS_MSG_BUFSIZ)
		return -EINVAL;

	if (copy_from_user(kcs_bmc->data_out, buf, count))
		return -EFAULT;

	spin_lock_irqsave(&kcs_bmc->lock, flags);
	if (kcs_bmc->kcs_phase == KCS_PHASE_READ) {
		kcs_bmc->data_out_idx = 1;
		kcs_bmc->data_out_len = count;
		kcs_outb(kcs_bmc, kcs_bmc->data_out[0], kcs_bmc->odr);
	}
	spin_unlock_irqrestore(&kcs_bmc->lock, flags);

	return count;
}

static long kcs_bmc_ioctl(struct file *filp, unsigned int cmd,
			  unsigned long arg)
{
	long ret = 0;
	struct kcs_bmc *kcs_bmc = file_kcs_bmc(filp);

	switch (cmd) {
	case KCS_BMC_IOCTL_SMS_ATN:
		kcs_set_atn(kcs_bmc, arg);
		break;

	case KCS_BMC_IOCTL_FORCE_ABORT:
		kcs_force_abort(kcs_bmc);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int kcs_bmc_release(struct inode *inode, struct file *filp)
{
	unsigned long flags;
	struct kcs_bmc *kcs_bmc = file_kcs_bmc(filp);

	spin_lock_irqsave(&kcs_bmc->lock, flags);
	kcs_bmc->running = 0;
	spin_unlock_irqrestore(&kcs_bmc->lock, flags);

	return 0;
}

static const struct file_operations kcs_bmc_fops = {
	.owner          = THIS_MODULE,
	.open           = kcs_bmc_open,
	.read           = kcs_bmc_read,
	.write          = kcs_bmc_write,
	.release        = kcs_bmc_release,
	.poll           = kcs_bmc_poll,
	.unlocked_ioctl = kcs_bmc_ioctl,
};

static int kcs_bmc_probe(struct platform_device *pdev)
{
	struct kcs_bmc *kcs_bmc;
	struct device *dev;
	const struct kcs_ioreg *ioreg;
	u32 chan, addr;
	int rc;

	dev = &pdev->dev;

	kcs_bmc = devm_kzalloc(dev, sizeof(*kcs_bmc), GFP_KERNEL);
	if (!kcs_bmc)
		return -ENOMEM;

	rc = of_property_read_u32(dev->of_node, "kcs_chan", &chan);
	if ((rc != 0) || (chan == 0 || chan > KCS_CHANNEL_MAX)) {
		dev_err(dev, "no valid 'kcs_chan' configured\n");
		return -ENODEV;
	}

	rc = of_property_read_u32(dev->of_node, "kcs_addr", &addr);
	if (rc) {
		dev_err(dev, "no valid 'kcs_addr' configured\n");
		return -ENODEV;
	}

	kcs_bmc->map = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(kcs_bmc->map)) {
		dev_err(dev, "Couldn't get regmap\n");
		return -ENODEV;
	}

	spin_lock_init(&kcs_bmc->lock);
	kcs_bmc->chan = chan;

	ioreg = &kcs_channel_ioregs[chan - 1];
	kcs_bmc->idr  = ioreg->idr;
	kcs_bmc->odr  = ioreg->odr;
	kcs_bmc->str  = ioreg->str;

	init_waitqueue_head(&kcs_bmc->queue);
	kcs_bmc->data_in  = devm_kmalloc(dev, KCS_MSG_BUFSIZ, GFP_KERNEL);
	kcs_bmc->data_out = devm_kmalloc(dev, KCS_MSG_BUFSIZ, GFP_KERNEL);
	if (kcs_bmc->data_in == NULL || kcs_bmc->data_out == NULL) {
		dev_err(dev, "Failed to allocate data buffers\n");
		return -ENOMEM;
	}

	snprintf(kcs_bmc->name, sizeof(kcs_bmc->name), "ipmi-kcs%u", chan);
	kcs_bmc->miscdev.minor = MISC_DYNAMIC_MINOR;
	kcs_bmc->miscdev.name = kcs_bmc->name;
	kcs_bmc->miscdev.fops = &kcs_bmc_fops;
	rc = misc_register(&kcs_bmc->miscdev);
	if (rc) {
		dev_err(dev, "Unable to register device\n");
		return rc;
	}

	kcs_set_addr(kcs_bmc, addr);
	kcs_enable_channel(kcs_bmc, 1);

	rc = kcs_bmc_config_irq(kcs_bmc, pdev);
	if (rc) {
		dev_err(dev, "Failed to configure IRQ\n");
		misc_deregister(&kcs_bmc->miscdev);
		return rc;
	}

	dev_set_drvdata(&pdev->dev, kcs_bmc);

	dev_info(dev, "addr=0x%x, idr=0x%x, odr=0x%x, str=0x%x\n",
			addr,
			kcs_bmc->idr, kcs_bmc->odr, kcs_bmc->str);

	return 0;
}

static int kcs_bmc_remove(struct platform_device *pdev)
{
	struct kcs_bmc *kcs_bmc = dev_get_drvdata(&pdev->dev);

	misc_deregister(&kcs_bmc->miscdev);

	return 0;
}

static const struct of_device_id kcs_bmc_match[] = {
	{ .compatible = "aspeed,ast2400-kcs-bmc" },
	{ .compatible = "aspeed,ast2500-kcs-bmc" },
	{ },
};

static struct platform_driver kcs_bmc_driver = {
	.driver = {
		.name           = DEVICE_NAME,
		.of_match_table = kcs_bmc_match,
	},
	.probe = kcs_bmc_probe,
	.remove = kcs_bmc_remove,
};

module_platform_driver(kcs_bmc_driver);

MODULE_DEVICE_TABLE(of, kcs_bmc_match);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Haiyue Wang <haiyue.wang@linux.intel.com>");
MODULE_DESCRIPTION("Linux device interface to the IPMI KCS interface");
