/***************************************************************************
 *
 * Copyright (C) 2004-2008 SMSC
 * Copyright (C) 2005-2008 ARM
 * Copyright (C) 2015-2016 MICROCHIP
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 ***************************************************************************
 * This file was originally derived from ../smsc/smsc911x.c
 * which was
 *
 * Rewritten, heavily based on smsc911x simple driver by SMSC.
 * Partly uses io macros from smc91x.c by Nicolas Pitre
 *
 * Supported devices:
 *   LAN9352
 *
 * May support
 *   LAN9250, LAN9311, LAN9312
 *   But these devices were unable to be tested
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/crc32.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/bug.h>
#include <linux/bitops.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/swab.h>
#include <linux/phy.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_net.h>
#include <linux/acpi.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>

#include "mchp9352.h"

#define MCHP_CHIPNAME		"mchp9352"
#define MCHP_MDIONAME		"mchp9352-mdio"
#define MCHP_DRV_VERSION	"2016-01-25"

MODULE_LICENSE("GPL");
MODULE_VERSION(MCHP_DRV_VERSION);
MODULE_ALIAS("platform:mchp9352");

#if USE_DEBUG > 0
static int debug = 16;
#else
static int debug = 3;
#endif

module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level (0=none,...,16=all)");

struct mchp9352_data;

struct mchp9352_ops {
	u32 (*reg_read)(struct mchp9352_data *pdata, u32 reg);
	void (*reg_write)(struct mchp9352_data *pdata, u32 reg, u32 val);
	void (*rx_readfifo)(struct mchp9352_data *pdata,
			    unsigned int *buf, unsigned int wordcount);
	void (*tx_writefifo)(struct mchp9352_data *pdata,
			     unsigned int *buf, unsigned int wordcount);
};

#define MCHP9352_NUM_SUPPLIES 2

struct mchp9352_data {
	void __iomem *ioaddr;

	unsigned int idrev;

	/* device configuration (copied from platform_data during probe) */
	struct mchp9352_platform_config config;

	/* This needs to be acquired before calling any of below:
	 * mchp9352_mac_read(), mchp9352_mac_write()
	 */
	spinlock_t mac_lock;

	/* spinlock to ensure register accesses are serialised */
	spinlock_t dev_lock;

	struct phy_device *phy_dev;
	struct mii_bus *mii_bus;
	int phy_irq[PHY_MAX_ADDR];
	int last_duplex;
	int last_carrier;

	u32 msg_enable;
	unsigned int gpio_setting;
	unsigned int gpio_orig_setting;
	struct net_device *dev;
	struct napi_struct napi;

	unsigned int software_irq_signal;

#define MIN_PACKET_SIZE (64)
	char loopback_tx_pkt[MIN_PACKET_SIZE];
	char loopback_rx_pkt[MIN_PACKET_SIZE];
	unsigned int resetcount;

	/* Members for Multicast filter workaround */
	unsigned int multicast_update_pending;
	unsigned int set_bits_mask;
	unsigned int clear_bits_mask;
	unsigned int hashhi;
	unsigned int hashlo;

	/* register access functions */
	const struct mchp9352_ops *ops;

	/* regulators */
	struct regulator_bulk_data supplies[MCHP9352_NUM_SUPPLIES];

	/* clock */
	struct clk *clk;
};

/* Easy access to information */
#define __mchp_shift(pdata, reg) ((reg) << ((pdata)->config.shift))

static inline u32 __mchp9352_reg_read(struct mchp9352_data *pdata, u32 reg)
{
	if (pdata->config.flags & MCHP9352_USE_32BIT)
		return readl(pdata->ioaddr + reg);

	if (pdata->config.flags & MCHP9352_USE_16BIT)
		return ((readw(pdata->ioaddr + reg) & 0xFFFF) |
			((readw(pdata->ioaddr + reg + 2) & 0xFFFF) << 16));

	WARN_ON(1);
	return 0;
}

static inline u32
__mchp9352_reg_read_shift(struct mchp9352_data *pdata, u32 reg)
{
	if (pdata->config.flags & MCHP9352_USE_32BIT)
		return readl(pdata->ioaddr + __mchp_shift(pdata, reg));

	if (pdata->config.flags & MCHP9352_USE_16BIT)
		return (readw(pdata->ioaddr +
				__mchp_shift(pdata, reg)) & 0xFFFF) |
			((readw(pdata->ioaddr +
			__mchp_shift(pdata, reg + 2)) & 0xFFFF) << 16);

	WARN_ON(1);
	return 0;
}

static inline u32 mchp9352_reg_read(struct mchp9352_data *pdata, u32 reg)
{
	u32 data;
	unsigned long flags;

	spin_lock_irqsave(&pdata->dev_lock, flags);
	data = pdata->ops->reg_read(pdata, reg);
	spin_unlock_irqrestore(&pdata->dev_lock, flags);

	return data;
}

static inline void __mchp9352_reg_write(struct mchp9352_data *pdata, u32 reg,
					u32 val)
{
	if (pdata->config.flags & MCHP9352_USE_32BIT) {
		writel(val, pdata->ioaddr + reg);
		return;
	}

	if (pdata->config.flags & MCHP9352_USE_16BIT) {
		writew(val & 0xFFFF, pdata->ioaddr + reg);
		writew((val >> 16) & 0xFFFF, pdata->ioaddr + reg + 2);
		return;
	}

	WARN_ON(1);
}

static inline void
__mchp9352_reg_write_shift(struct mchp9352_data *pdata, u32 reg, u32 val)
{
	if (pdata->config.flags & MCHP9352_USE_32BIT) {
		writel(val, pdata->ioaddr + __mchp_shift(pdata, reg));
		return;
	}

	if (pdata->config.flags & MCHP9352_USE_16BIT) {
		writew(val & 0xFFFF,
		       pdata->ioaddr + __mchp_shift(pdata, reg));
		writew((val >> 16) & 0xFFFF,
		       pdata->ioaddr + __mchp_shift(pdata, reg + 2));
		return;
	}

	WARN_ON(1);
}

static inline void mchp9352_reg_write(struct mchp9352_data *pdata, u32 reg,
				      u32 val)
{
	unsigned long flags;

	spin_lock_irqsave(&pdata->dev_lock, flags);
	pdata->ops->reg_write(pdata, reg, val);
	spin_unlock_irqrestore(&pdata->dev_lock, flags);
}

/* Writes a packet to the TX_DATA_FIFO */
static inline void
mchp9352_tx_writefifo(struct mchp9352_data *pdata, unsigned int *buf,
		      unsigned int wordcount)
{
	unsigned long flags;

	spin_lock_irqsave(&pdata->dev_lock, flags);

	if (pdata->config.flags & MCHP9352_SWAP_FIFO) {
		while (wordcount--)
			__mchp9352_reg_write(pdata, TX_DATA_FIFO,
					     swab32(*buf++));
		goto out;
	}

	if (pdata->config.flags & MCHP9352_USE_32BIT) {
		iowrite32_rep(pdata->ioaddr + TX_DATA_FIFO, buf, wordcount);
		goto out;
	}

	if (pdata->config.flags & MCHP9352_USE_16BIT) {
		while (wordcount--)
			__mchp9352_reg_write(pdata, TX_DATA_FIFO, *buf++);
		goto out;
	}

	WARN_ON(1);
out:
	spin_unlock_irqrestore(&pdata->dev_lock, flags);
}

/* Writes a packet to the TX_DATA_FIFO - shifted version */
static inline void
mchp9352_tx_writefifo_shift(struct mchp9352_data *pdata, unsigned int *buf,
			    unsigned int wordcount)
{
	unsigned long flags;

	spin_lock_irqsave(&pdata->dev_lock, flags);

	if (pdata->config.flags & MCHP9352_SWAP_FIFO) {
		while (wordcount--)
			__mchp9352_reg_write_shift(pdata, TX_DATA_FIFO,
						   swab32(*buf++));
		goto out;
	}

	if (pdata->config.flags & MCHP9352_USE_32BIT) {
		iowrite32_rep(pdata->ioaddr +
			      __mchp_shift(pdata, TX_DATA_FIFO),
			      buf, wordcount);
		goto out;
	}

	if (pdata->config.flags & MCHP9352_USE_16BIT) {
		while (wordcount--)
			__mchp9352_reg_write_shift(pdata,
						   TX_DATA_FIFO, *buf++);
		goto out;
	}

	WARN_ON(1);
out:
	spin_unlock_irqrestore(&pdata->dev_lock, flags);
}

/* Reads a packet out of the RX_DATA_FIFO */
static inline void
mchp9352_rx_readfifo(struct mchp9352_data *pdata, unsigned int *buf,
		     unsigned int wordcount)
{
	unsigned long flags;

	spin_lock_irqsave(&pdata->dev_lock, flags);

	if (pdata->config.flags & MCHP9352_SWAP_FIFO) {
		while (wordcount--)
			*buf++ = swab32(__mchp9352_reg_read(pdata,
							    RX_DATA_FIFO));
		goto out;
	}

	if (pdata->config.flags & MCHP9352_USE_32BIT) {
		ioread32_rep(pdata->ioaddr + RX_DATA_FIFO, buf, wordcount);
		goto out;
	}

	if (pdata->config.flags & MCHP9352_USE_16BIT) {
		while (wordcount--)
			*buf++ = __mchp9352_reg_read(pdata, RX_DATA_FIFO);
		goto out;
	}

	WARN_ON(1);
out:
	spin_unlock_irqrestore(&pdata->dev_lock, flags);
}

/* Reads a packet out of the RX_DATA_FIFO - shifted version */
static inline void
mchp9352_rx_readfifo_shift(struct mchp9352_data *pdata, unsigned int *buf,
			   unsigned int wordcount)
{
	unsigned long flags;

	spin_lock_irqsave(&pdata->dev_lock, flags);

	if (pdata->config.flags & MCHP9352_SWAP_FIFO) {
		while (wordcount--)
			*buf++ = swab32(__mchp9352_reg_read_shift(pdata,
							    RX_DATA_FIFO));
		goto out;
	}

	if (pdata->config.flags & MCHP9352_USE_32BIT) {
		ioread32_rep(pdata->ioaddr +
			     __mchp_shift(pdata, RX_DATA_FIFO),
			     buf, wordcount);
		goto out;
	}

	if (pdata->config.flags & MCHP9352_USE_16BIT) {
		while (wordcount--)
			*buf++ = __mchp9352_reg_read_shift(pdata,
								RX_DATA_FIFO);
		goto out;
	}

	WARN_ON(1);
out:
	spin_unlock_irqrestore(&pdata->dev_lock, flags);
}

/* enable regulator and clock resources. */
static int mchp9352_enable_resources(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct mchp9352_data *pdata = netdev_priv(ndev);
	int ret = 0;

	ret = regulator_bulk_enable(ARRAY_SIZE(pdata->supplies),
				    pdata->supplies);
	if (ret)
		netdev_err(ndev, "failed to enable regulators %d\n",
			   ret);

	if (!IS_ERR(pdata->clk)) {
		ret = clk_prepare_enable(pdata->clk);
		if (ret < 0)
			netdev_err(ndev, "failed to enable clock %d\n", ret);
	}

	return ret;
}

/* disable resources, currently just regulators. */
static int mchp9352_disable_resources(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct mchp9352_data *pdata = netdev_priv(ndev);
	int ret = 0;

	ret = regulator_bulk_disable(ARRAY_SIZE(pdata->supplies),
				     pdata->supplies);

	if (!IS_ERR(pdata->clk))
		clk_disable_unprepare(pdata->clk);

	return ret;
}

/* Request resources, currently just regulators.
 *
 * The SMSC911x has two power pins: vddvario and vdd33a, in designs where
 * these are not always-on we need to request regulators to be turned on
 * before we can try to access the device registers.
 */
static int mchp9352_request_resources(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct mchp9352_data *pdata = netdev_priv(ndev);
	int ret = 0;

	/* Request regulators */
	pdata->supplies[0].supply = "vdd33a";
	pdata->supplies[1].supply = "vddvario";
	ret = regulator_bulk_get(&pdev->dev,
				 ARRAY_SIZE(pdata->supplies),
				 pdata->supplies);
	if (ret)
		netdev_err(ndev, "couldn't get regulators %d\n",
			   ret);

	/* Request clock */
	pdata->clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(pdata->clk))
		dev_dbg(&pdev->dev, "couldn't get clock %li\n",
			PTR_ERR(pdata->clk));

	return ret;
}

/* Free resources, currently just regulators. */
static void mchp9352_free_resources(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct mchp9352_data *pdata = netdev_priv(ndev);

	/* Free regulators */
	regulator_bulk_free(ARRAY_SIZE(pdata->supplies),
			    pdata->supplies);

	/* Free clock */
	if (!IS_ERR(pdata->clk)) {
		clk_put(pdata->clk);
		pdata->clk = NULL;
	}
}

/* waits for MAC not busy, with timeout.  Only called by mchp9352_mac_read
 * and mchp9352_mac_write, so assumes mac_lock is held
 */
static int mchp9352_mac_complete(struct mchp9352_data *pdata)
{
	int i;
	u32 val;

	MCHP_ASSERT_MAC_LOCK(pdata);

	for (i = 0; i < 40; i++) {
		val = mchp9352_reg_read(pdata, MAC_CSR_CMD);
		if (!(val & MAC_CSR_CMD_CSR_BUSY_))
			return 0;
	}
	MCHP_WARN(pdata, hw,
		  "Timed out waiting for MAC not BUSY. MAC_CSR_CMD: 0x%08X",
		  val);
	return -EIO;
}

/* Fetches a MAC register value. Assumes mac_lock is acquired */
static u32 mchp9352_mac_read(struct mchp9352_data *pdata, unsigned int offset)
{
	unsigned int temp;

	MCHP_ASSERT_MAC_LOCK(pdata);

	temp = mchp9352_reg_read(pdata, MAC_CSR_CMD);
	if (unlikely(temp & MAC_CSR_CMD_CSR_BUSY_)) {
		MCHP_WARN(pdata, hw, "MAC busy at entry");
		return 0xFFFFFFFF;
	}

	/* Send the MAC cmd */
	mchp9352_reg_write(pdata, MAC_CSR_CMD, ((offset & 0xFF) |
		MAC_CSR_CMD_CSR_BUSY_ | MAC_CSR_CMD_R_NOT_W_));

	/* Workaround for hardware read-after-write restriction */
	temp = mchp9352_reg_read(pdata, BYTE_TEST);

	/* Wait for the read to complete */
	if (likely(mchp9352_mac_complete(pdata) == 0))
		return mchp9352_reg_read(pdata, MAC_CSR_DATA);

	MCHP_WARN(pdata, hw, "MAC busy after read");
	return 0xFFFFFFFF;
}

/* Set a mac register, mac_lock must be acquired before calling */
static void mchp9352_mac_write(struct mchp9352_data *pdata,
			       unsigned int offset, u32 val)
{
	unsigned int temp;

	MCHP_ASSERT_MAC_LOCK(pdata);

	temp = mchp9352_reg_read(pdata, MAC_CSR_CMD);
	if (unlikely(temp & MAC_CSR_CMD_CSR_BUSY_)) {
		MCHP_WARN(pdata, hw,
			  "mchp9352_mac_write failed, MAC busy at entry");
		return;
	}

	/* Send data to write */
	mchp9352_reg_write(pdata, MAC_CSR_DATA, val);

	/* Write the actual data */
	mchp9352_reg_write(pdata, MAC_CSR_CMD, ((offset & 0xFF) |
		MAC_CSR_CMD_CSR_BUSY_));

	/* Workaround for hardware read-after-write restriction */
	temp = mchp9352_reg_read(pdata, BYTE_TEST);

	/* Wait for the write to complete */
	if (likely(mchp9352_mac_complete(pdata) == 0))
		return;

	MCHP_WARN(pdata, hw, "mchp9352_mac_write failed, MAC busy after write");
}

/* Get a phy register */
static int mchp9352_mii_read(struct mii_bus *bus, int phyaddr, int regidx)
{
	struct mchp9352_data *pdata = (struct mchp9352_data *)bus->priv;
	unsigned long flags;
	unsigned int addr;
	int i, reg;

	spin_lock_irqsave(&pdata->mac_lock, flags);

	/* Confirm MII not busy */
	if (unlikely(mchp9352_mac_read(pdata, MII_ACC) & MII_ACC_MII_BUSY_)) {
		MCHP_WARN(pdata, hw, "MII is busy in mchp9352_mii_read???");
		reg = -EIO;
		goto out;
	}

	/* Set the address, index & direction (read from PHY) */
	addr = ((phyaddr & 0x1F) << 11) | ((regidx & 0x1F) << 6);
	mchp9352_mac_write(pdata, MII_ACC, addr);

	/* Wait for read to complete w/ timeout */
	for (i = 0; i < 100; i++)
		if (!(mchp9352_mac_read(pdata, MII_ACC) & MII_ACC_MII_BUSY_)) {
			reg = mchp9352_mac_read(pdata, MII_DATA);
			goto out;
		}

	MCHP_WARN(pdata, hw, "Timed out waiting for MII read to finish");
	reg = -EIO;

out:
	spin_unlock_irqrestore(&pdata->mac_lock, flags);
	return reg;
}

/* Set a phy register */
static int mchp9352_mii_write(struct mii_bus *bus, int phyaddr, int regidx,
			      u16 val)
{
	struct mchp9352_data *pdata = (struct mchp9352_data *)bus->priv;
	unsigned long flags;
	unsigned int addr;
	int i, reg;

	spin_lock_irqsave(&pdata->mac_lock, flags);

	/* Confirm MII not busy */
	if (unlikely(mchp9352_mac_read(pdata, MII_ACC) & MII_ACC_MII_BUSY_)) {
		MCHP_WARN(pdata, hw, "MII is busy in mchp9352_mii_write???");
		reg = -EIO;
		goto out;
	}

	/* Put the data to write in the MAC */
	mchp9352_mac_write(pdata, MII_DATA, val);

	/* Set the address, index & direction (write to PHY) */
	addr = ((phyaddr & 0x1F) << 11) | ((regidx & 0x1F) << 6) |
		MII_ACC_MII_WRITE_;
	mchp9352_mac_write(pdata, MII_ACC, addr);

	/* Wait for write to complete w/ timeout */
	for (i = 0; i < 100; i++)
		if (!(mchp9352_mac_read(pdata, MII_ACC) & MII_ACC_MII_BUSY_)) {
			reg = 0;
			goto out;
		}

	MCHP_WARN(pdata, hw, "Timed out waiting for MII write to finish");
	reg = -EIO;

out:
	spin_unlock_irqrestore(&pdata->mac_lock, flags);
	return reg;
}

/* Fetches a tx status out of the status fifo */
static unsigned int mchp9352_tx_get_txstatus(struct mchp9352_data *pdata)
{
	unsigned int result =
	    mchp9352_reg_read(pdata, TX_FIFO_INF) & TX_FIFO_INF_TSUSED_;

	if (result != 0)
		result = mchp9352_reg_read(pdata, TX_STATUS_FIFO);

	return result;
}

/* Fetches the next rx status */
static unsigned int mchp9352_rx_get_rxstatus(struct mchp9352_data *pdata)
{
	unsigned int result =
	    mchp9352_reg_read(pdata, RX_FIFO_INF) & RX_FIFO_INF_RXSUSED_;

	if (result != 0)
		result = mchp9352_reg_read(pdata, RX_STATUS_FIFO);

	return result;
}

static int mchp9352_phy_check_loopbackpkt(struct mchp9352_data *pdata)
{
	unsigned int tries;
	u32 wrsz;
	u32 rdsz;
	ulong bufp;

	for (tries = 0; tries < 10; tries++) {
		unsigned int txcmd_a;
		unsigned int txcmd_b;
		unsigned int status;
		unsigned int pktlength;
		unsigned int i;

		/* Zero-out rx packet memory */
		memset(pdata->loopback_rx_pkt, 0, MIN_PACKET_SIZE);

		/* Write tx packet to 118 */
		txcmd_a = (u32)((ulong)pdata->loopback_tx_pkt & 0x03) << 16;
		txcmd_a |= TX_CMD_A_FIRST_SEG_ | TX_CMD_A_LAST_SEG_;
		txcmd_a |= MIN_PACKET_SIZE;

		txcmd_b = MIN_PACKET_SIZE << 16 | MIN_PACKET_SIZE;

		mchp9352_reg_write(pdata, TX_DATA_FIFO, txcmd_a);
		mchp9352_reg_write(pdata, TX_DATA_FIFO, txcmd_b);

		bufp = (ulong)pdata->loopback_tx_pkt & (~0x3);
		wrsz = MIN_PACKET_SIZE + 3;
		wrsz += (u32)((ulong)pdata->loopback_tx_pkt & 0x3);
		wrsz >>= 2;

		pdata->ops->tx_writefifo(pdata, (unsigned int *)bufp, wrsz);

		/* Wait till transmit is done */
		i = 60;
		do {
			udelay(5);
			status = mchp9352_tx_get_txstatus(pdata);
		} while ((i--) && (!status));

		if (!status) {
			MCHP_WARN(pdata, hw,
				  "Failed to transmit during loopback test");
			continue;
		}
		if (status & TX_STS_ES_) {
			MCHP_WARN(pdata, hw,
				  "Transmit encountered errors during loopback test");
			continue;
		}

		/* Wait till receive is done */
		i = 60;
		do {
			udelay(5);
			status = mchp9352_rx_get_rxstatus(pdata);
		} while ((i--) && (!status));

		if (!status) {
			MCHP_WARN(pdata, hw,
				  "Failed to receive during loopback test");
			continue;
		}
		if (status & RX_STS_ES_) {
			MCHP_WARN(pdata, hw,
				  "Receive encountered errors during loopback test");
			continue;
		}

		pktlength = ((status & 0x3FFF0000UL) >> 16);
		bufp = (ulong)pdata->loopback_rx_pkt;
		rdsz = pktlength + 3;
		rdsz += (u32)((ulong)pdata->loopback_rx_pkt & 0x3);
		rdsz >>= 2;

		pdata->ops->rx_readfifo(pdata, (unsigned int *)bufp, rdsz);

		if (pktlength != (MIN_PACKET_SIZE + 4)) {
			MCHP_WARN(pdata, hw,
				  "Unexpected packet size during loop back test, size=%d, will retry",
				  pktlength);
		} else {
			unsigned int j;
			int mismatch = 0;

			for (j = 0; j < MIN_PACKET_SIZE; j++) {
				if (pdata->loopback_tx_pkt[j]
				    != pdata->loopback_rx_pkt[j]) {
					mismatch = 1;
					break;
				}
			}
			if (!mismatch) {
				MCHP_TRACE(pdata, hw,
					   "Successfully verified loopback packet");
				return 0;
			}
			MCHP_WARN(pdata, hw,
				  "Data mismatch during loop back test, will retry");
		}
	}

	return -EIO;
}

static int mchp9352_phy_reset(struct mchp9352_data *pdata)
{
	unsigned int temp;
	unsigned int i = 100000;

	temp = mchp9352_reg_read(pdata, PMT_CTRL);
	mchp9352_reg_write(pdata, PMT_CTRL, temp | PMT_CTRL_PHY_RST_);
	do {
		usleep_range(1000, 10000);
		temp = mchp9352_reg_read(pdata, PMT_CTRL);
	} while ((i--) && (temp & PMT_CTRL_PHY_RST_));

	if (unlikely(temp & PMT_CTRL_PHY_RST_)) {
		MCHP_WARN(pdata, hw, "PHY reset failed to complete");
		return -EIO;
	}
	/* Extra delay required because the phy may not be completed with
	 * its reset when BMCR_RESET is cleared. Specs say 256 uS is
	 * enough delay but using 1ms here to be safe
	 */
	usleep_range(1000, 10000);

	return 0;
}

static int mchp9352_phy_loopbacktest(struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);
	struct phy_device *phy_dev = pdata->phy_dev;
	int result = -EIO;
	unsigned int i, val;
	unsigned long flags;

	/* Initialise tx packet using broadcast destination address */
	eth_broadcast_addr(pdata->loopback_tx_pkt);

	/* Use incrementing source address */
	for (i = 6; i < 12; i++)
		pdata->loopback_tx_pkt[i] = (char)i;

	/* Set length type field */
	pdata->loopback_tx_pkt[12] = 0x00;
	pdata->loopback_tx_pkt[13] = 0x00;

	for (i = 14; i < MIN_PACKET_SIZE; i++)
		pdata->loopback_tx_pkt[i] = (char)i;

	val = mchp9352_reg_read(pdata, HW_CFG);
	val &= HW_CFG_TX_FIF_SZ_(0xFFFFFFFFUL);
	val |= HW_CFG_SF_;/* on many parts this must be set */
	mchp9352_reg_write(pdata, HW_CFG, val);

	mchp9352_reg_write(pdata, TX_CFG, TX_CFG_TX_ON_);
	mchp9352_reg_write(pdata, RX_CFG,
			   (u32)((ulong)pdata->loopback_rx_pkt & 0x03) << 8);

	for (i = 0; i < 10; i++) {
		/* Set PHY to 10/FD, no ANEG, and loopback mode */
		mchp9352_mii_write(phy_dev->mdio.bus, phy_dev->mdio.addr,
				   MII_BMCR, BMCR_LOOPBACK | BMCR_FULLDPLX);

		/* Enable MAC tx/rx, FD */
		spin_lock_irqsave(&pdata->mac_lock, flags);
		mchp9352_mac_write(pdata, MAC_CR, MAC_CR_FDPX_
				   | MAC_CR_TXEN_ | MAC_CR_RXEN_);
		spin_unlock_irqrestore(&pdata->mac_lock, flags);

		if (mchp9352_phy_check_loopbackpkt(pdata) == 0) {
			result = 0;
			break;
		}
		pdata->resetcount++;

		/* Disable MAC rx */
		spin_lock_irqsave(&pdata->mac_lock, flags);
		mchp9352_mac_write(pdata, MAC_CR, 0);
		spin_unlock_irqrestore(&pdata->mac_lock, flags);

		mchp9352_phy_reset(pdata);
	}

	/* Disable MAC */
	spin_lock_irqsave(&pdata->mac_lock, flags);
	mchp9352_mac_write(pdata, MAC_CR, 0);
	spin_unlock_irqrestore(&pdata->mac_lock, flags);

	/* Cancel PHY loopback mode */
	mchp9352_mii_write(phy_dev->mdio.bus, phy_dev->mdio.addr, MII_BMCR, 0);

	mchp9352_reg_write(pdata, TX_CFG, 0);
	mchp9352_reg_write(pdata, RX_CFG, 0);

	return result;
}

static void mchp9352_phy_update_flowcontrol(struct mchp9352_data *pdata)
{
	struct phy_device *phy_dev = pdata->phy_dev;
	u32 afc = mchp9352_reg_read(pdata, AFC_CFG);
	u32 flow;
	unsigned long flags;

	if (phy_dev->duplex == DUPLEX_FULL) {
		u16 lcladv = phy_read(phy_dev, MII_ADVERTISE);
		u16 rmtadv = phy_read(phy_dev, MII_LPA);
		u8 cap = mii_resolve_flowctrl_fdx(lcladv, rmtadv);

		if (cap & FLOW_CTRL_RX)
			flow = 0xFFFF0002;
		else
			flow = 0;

		if (cap & FLOW_CTRL_TX)
			afc |= 0xF;
		else
			afc &= ~0xF;

		MCHP_TRACE(pdata, hw, "rx pause %s, tx pause %s",
			   (cap & FLOW_CTRL_RX ? "enabled" : "disabled"),
			   (cap & FLOW_CTRL_TX ? "enabled" : "disabled"));
	} else {
		MCHP_TRACE(pdata, hw, "half duplex");
		flow = 0;
		afc |= 0xF;
	}

	spin_lock_irqsave(&pdata->mac_lock, flags);
	mchp9352_mac_write(pdata, FLOW, flow);
	spin_unlock_irqrestore(&pdata->mac_lock, flags);

	mchp9352_reg_write(pdata, AFC_CFG, afc);
}

/* Update link mode if anything has changed.  Called periodically when the
 * PHY is in polling mode, even if nothing has changed.
 */
static void mchp9352_phy_adjust_link(struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);
	struct phy_device *phy_dev = pdata->phy_dev;
	unsigned long flags;

	if (phy_dev->duplex != pdata->last_duplex) {
		unsigned int mac_cr;

		MCHP_TRACE(pdata, hw, "duplex state has changed");

		spin_lock_irqsave(&pdata->mac_lock, flags);
		mac_cr = mchp9352_mac_read(pdata, MAC_CR);
		if (phy_dev->duplex) {
			MCHP_TRACE(pdata, hw,
				   "configuring for full duplex mode");
			mac_cr |= MAC_CR_FDPX_;
		} else {
			MCHP_TRACE(pdata, hw,
				   "configuring for half duplex mode");
			mac_cr &= ~MAC_CR_FDPX_;
		}
		mchp9352_mac_write(pdata, MAC_CR, mac_cr);
		spin_unlock_irqrestore(&pdata->mac_lock, flags);

		mchp9352_phy_update_flowcontrol(pdata);
		pdata->last_duplex = phy_dev->duplex;
	}
}

static int mchp9352_mii_probe(struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);
	struct phy_device *phydev = NULL;
	int ret;

	/* find the first phy */
	phydev = phy_find_first(pdata->mii_bus);
	if (!phydev) {
		netdev_err(dev, "no PHY found\n");
		return -ENODEV;
	}

	MCHP_TRACE(pdata, probe, "PHY: addr %d, phy_id 0x%08X",
		   phydev->mdio.addr, phydev->phy_id);

	ret = phy_connect_direct(dev, phydev, &mchp9352_phy_adjust_link,
				 pdata->config.phy_interface);

	if (ret) {
		netdev_err(dev, "Could not attach to PHY\n");
		return ret;
	}

	phy_attached_info(phydev);

	/* mask with MAC supported features */
	phydev->supported &= (PHY_BASIC_FEATURES | SUPPORTED_Pause |
			      SUPPORTED_Asym_Pause);
	phydev->advertising = phydev->supported;

	pdata->phy_dev = phydev;
	pdata->last_duplex = -1;
	pdata->last_carrier = -1;

	if (mchp9352_phy_loopbacktest(dev) < 0) {
		MCHP_WARN(pdata, hw, "Failed Loop Back Test");
		phy_disconnect(phydev);
		return -ENODEV;
	}
	MCHP_TRACE(pdata, hw, "Passed Loop Back Test");

	MCHP_TRACE(pdata, hw, "phy initialised successfully");
	return 0;
}

static int mchp9352_mii_init(struct platform_device *pdev,
			     struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);
	int err = -ENXIO;

	pdata->mii_bus = mdiobus_alloc();
	if (!pdata->mii_bus) {
		err = -ENOMEM;
		goto err_out_1;
	}

	pdata->mii_bus->name = MCHP_MDIONAME;
	snprintf(pdata->mii_bus->id, MII_BUS_ID_SIZE, "%s-%x",
		 pdev->name, pdev->id);
	pdata->mii_bus->priv = pdata;
	pdata->mii_bus->read = mchp9352_mii_read;
	pdata->mii_bus->write = mchp9352_mii_write;
	memcpy(pdata->mii_bus->irq, pdata->phy_irq, sizeof(pdata->mii_bus));

	pdata->mii_bus->parent = &pdev->dev;

	/* Mask all PHYs except address 0 and 1 (internal) */
	pdata->mii_bus->phy_mask = ~(3 << 0);

	if (mdiobus_register(pdata->mii_bus)) {
		MCHP_WARN(pdata, probe, "Error registering mii bus");
		goto err_out_free_bus_2;
	}

	if (mchp9352_mii_probe(dev) < 0) {
		MCHP_WARN(pdata, probe, "Error registering mii bus");
		goto err_out_unregister_bus_3;
	}

	return 0;

err_out_unregister_bus_3:
	mdiobus_unregister(pdata->mii_bus);
err_out_free_bus_2:
	mdiobus_free(pdata->mii_bus);
err_out_1:
	return err;
}

/* Gets the number of tx statuses in the fifo */
static unsigned int mchp9352_tx_get_txstatcount(struct mchp9352_data *pdata)
{
	return (mchp9352_reg_read(pdata, TX_FIFO_INF)
		& TX_FIFO_INF_TSUSED_) >> 16;
}

/* Reads tx statuses and increments counters where necessary */
static void mchp9352_tx_update_txcounters(struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);
	unsigned int tx_stat;

	while ((tx_stat = mchp9352_tx_get_txstatus(pdata)) != 0) {
		if (unlikely(tx_stat & 0x80000000)) {
			/* In this driver the packet tag is used as the packet
			 * length. Since a packet length can never reach the
			 * size of 0x8000, this bit is reserved. It is worth
			 * noting that the "reserved bit" in the warning above
			 * does not reference a hardware defined reserved bit
			 * but rather a driver defined one.
			 */
			MCHP_WARN(pdata, hw, "Packet tag reserved bit is high");
		} else {
			if (unlikely(tx_stat & TX_STS_ES_)) {
				dev->stats.tx_errors++;
			} else {
				dev->stats.tx_packets++;
				dev->stats.tx_bytes += (tx_stat >> 16);
			}
			if (unlikely(tx_stat & TX_STS_EXCESS_COL_)) {
				dev->stats.collisions += 16;
				dev->stats.tx_aborted_errors += 1;
			} else {
				dev->stats.collisions +=
				    ((tx_stat >> 3) & 0xF);
			}
			if (unlikely(tx_stat & TX_STS_LOST_CARRIER_))
				dev->stats.tx_carrier_errors += 1;
			if (unlikely(tx_stat & TX_STS_LATE_COL_)) {
				dev->stats.collisions++;
				dev->stats.tx_aborted_errors++;
			}
		}
	}
}

/* Increments the Rx error counters */
static void
mchp9352_rx_counterrors(struct net_device *dev, unsigned int rxstat)
{
	int crc_err = 0;

	if (unlikely(rxstat & RX_STS_ES_)) {
		dev->stats.rx_errors++;
		if (unlikely(rxstat & RX_STS_CRC_ERR_)) {
			dev->stats.rx_crc_errors++;
			crc_err = 1;
		}
	}
	if (likely(!crc_err)) {
		if (unlikely((rxstat & RX_STS_FRAME_TYPE_) &&
			     (rxstat & RX_STS_LENGTH_ERR_)))
			dev->stats.rx_length_errors++;
		if (rxstat & RX_STS_MCAST_)
			dev->stats.multicast++;
	}
}

/* Quickly dumps bad packets */
static void
mchp9352_rx_fastforward(struct mchp9352_data *pdata, unsigned int pktwords)
{
	if (likely(pktwords >= 4)) {
		unsigned int timeout = 500;
		unsigned int val;

		mchp9352_reg_write(pdata, RX_DP_CTRL, RX_DP_CTRL_RX_FFWD_);
		do {
			udelay(1);
			val = mchp9352_reg_read(pdata, RX_DP_CTRL);
		} while ((val & RX_DP_CTRL_RX_FFWD_) && --timeout);

		if (unlikely(timeout == 0))
			MCHP_WARN(pdata, hw,
				  "Timed out waiting for RX FFWD to finish, RX_DP_CTRL: 0x%08X",
				  val);
	} else {
		unsigned int temp;

		while (pktwords--)
			temp = mchp9352_reg_read(pdata, RX_DATA_FIFO);
	}
}

/* NAPI poll function */
static int mchp9352_poll(struct napi_struct *napi, int budget)
{
	struct mchp9352_data *pdata =
		container_of(napi, struct mchp9352_data, napi);
	struct net_device *dev = pdata->dev;
	int npackets = 0;

	while (npackets < budget) {
		unsigned int pktlength;
		unsigned int pktwords;
		struct sk_buff *skb;
		unsigned int rxstat = mchp9352_rx_get_rxstatus(pdata);

		if (!rxstat) {
			unsigned int temp;
			/* We processed all packets available.  Tell NAPI it can
			 * stop polling then re-enable rx interrupts
			 */
			mchp9352_reg_write(pdata, INT_STS, INT_STS_RSFL_);
			napi_complete(napi);
			temp = mchp9352_reg_read(pdata, INT_EN);
			temp |= INT_EN_RSFL_EN_;
			mchp9352_reg_write(pdata, INT_EN, temp);
			break;
		}

		/* Count packet for NAPI scheduling, even if it has an error.
		 * Error packets still require cycles to discard
		 */
		npackets++;

		pktlength = ((rxstat & 0x3FFF0000) >> 16);
		pktwords = (pktlength + NET_IP_ALIGN + 3) >> 2;
		mchp9352_rx_counterrors(dev, rxstat);

		if (unlikely(rxstat & RX_STS_ES_)) {
			MCHP_WARN(pdata, rx_err,
				  "Discarding packet with error bit set");
			/* Packet has an error, discard it and continue with
			 * the next
			 */
			mchp9352_rx_fastforward(pdata, pktwords);
			dev->stats.rx_dropped++;
			continue;
		}

		skb = netdev_alloc_skb(dev, pktwords << 2);
		if (unlikely(!skb)) {
			MCHP_WARN(pdata, rx_err,
				  "Unable to allocate skb for rx packet");
			/* Drop the packet and stop this polling iteration */
			mchp9352_rx_fastforward(pdata, pktwords);
			dev->stats.rx_dropped++;
			break;
		}

		pdata->ops->rx_readfifo(pdata,
				 (unsigned int *)skb->data, pktwords);

		/* Align IP on 16B boundary */
		skb_reserve(skb, NET_IP_ALIGN);
		skb_put(skb, pktlength - 4);
		skb->protocol = eth_type_trans(skb, dev);
		skb_checksum_none_assert(skb);
		netif_receive_skb(skb);

		/* Update counters */
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += (pktlength - 4);
	}

	/* Return total received packets */
	return npackets;
}

/* Returns hash bit number for given MAC address
 * Example:
 * 01 00 5E 00 00 01 -> returns bit number 31
 */
static unsigned int mchp9352_hash(char addr[ETH_ALEN])
{
	return (ether_crc(ETH_ALEN, addr) >> 26) & 0x3f;
}

static void mchp9352_rx_multicast_update(struct mchp9352_data *pdata)
{
	/* Performs the multicast & mac_cr update.  This is called when
	 * safe on the current hardware, and with the mac_lock held
	 */
	unsigned int mac_cr;

	MCHP_ASSERT_MAC_LOCK(pdata);

	mac_cr = mchp9352_mac_read(pdata, MAC_CR);
	mac_cr |= pdata->set_bits_mask;
	mac_cr &= ~(pdata->clear_bits_mask);
	mchp9352_mac_write(pdata, MAC_CR, mac_cr);
	mchp9352_mac_write(pdata, HASHH, pdata->hashhi);
	mchp9352_mac_write(pdata, HASHL, pdata->hashlo);
	MCHP_TRACE(pdata, hw, "maccr 0x%08X, HASHH 0x%08X, HASHL 0x%08X",
		   mac_cr, pdata->hashhi, pdata->hashlo);
}

static void mchp9352_rx_multicast_update_workaround(struct mchp9352_data *pdata)
{
	unsigned int mac_cr;

	/* This function is only called for older LAN911x devices
	 * (revA or revB), where MAC_CR, HASHH and HASHL should not
	 * be modified during Rx - newer devices immediately update the
	 * registers.
	 *
	 * This is called from interrupt context
	 */

	spin_lock(&pdata->mac_lock);

	/* Check Rx has stopped */
	if (mchp9352_mac_read(pdata, MAC_CR) & MAC_CR_RXEN_)
		MCHP_WARN(pdata, drv, "Rx not stopped");

	/* Perform the update - safe to do now Rx has stopped */
	mchp9352_rx_multicast_update(pdata);

	/* Re-enable Rx */
	mac_cr = mchp9352_mac_read(pdata, MAC_CR);
	mac_cr |= MAC_CR_RXEN_;
	mchp9352_mac_write(pdata, MAC_CR, mac_cr);

	pdata->multicast_update_pending = 0;

	spin_unlock(&pdata->mac_lock);
}

static int mchp9352_phy_general_power_up(struct mchp9352_data *pdata)
{
	int rc = 0;

	if (!pdata->phy_dev)
		return rc;

	/* If the internal PHY is in General Power-Down mode, all, except the
	 * management interface, is powered-down and stays in that condition as
	 * long as Phy register bit 0.11 is HIGH.
	 *
	 * In that case, clear the bit 0.11, so the PHY powers up and we can
	 * access to the phy registers.
	 */
	rc = phy_read(pdata->phy_dev, MII_BMCR);
	if (rc < 0) {
		MCHP_WARN(pdata, drv, "Failed reading PHY control reg");
		return rc;
	}

	/* If the PHY general power-down bit is not set is not necessary to
	 * disable the general power down-mode.
	 */
	if (rc & BMCR_PDOWN) {
		rc = phy_write(pdata->phy_dev, MII_BMCR, rc & ~BMCR_PDOWN);
		if (rc < 0) {
			MCHP_WARN(pdata, drv, "Failed writing PHY control reg");
			return rc;
		}

		usleep_range(1000, 1500);
	}

	return 0;
}

static int mchp9352_phy_enable_energy_detect(struct mchp9352_data *pdata)
{
	int rc = 0;

	if (!pdata->phy_dev)
		return rc;

	rc = phy_read(pdata->phy_dev, MII_LAN83C185_CTRL_STATUS);

	if (rc < 0) {
		MCHP_WARN(pdata, drv, "Failed reading PHY control reg");
		return rc;
	}

	/* Only enable if energy detect mode is already disabled */
	if (!(rc & MII_LAN83C185_EDPWRDOWN)) {
		/* Enable energy detect mode for this SMSC Transceivers */
		rc = phy_write(pdata->phy_dev, MII_LAN83C185_CTRL_STATUS,
			       rc | MII_LAN83C185_EDPWRDOWN);

		if (rc < 0) {
			MCHP_WARN(pdata, drv, "Failed writing PHY control reg");
			return rc;
		}
	}
	return 0;
}

static int mchp9352_wait_till_ready(struct mchp9352_data *pdata);

static int mchp9352_soft_reset(struct mchp9352_data *pdata)
{
	unsigned int timeout;
	unsigned int temp;
	int ret;

	/* Make sure to power-up the PHY chip before doing a reset, otherwise
	 * the reset fails.
	 */
	ret = mchp9352_phy_general_power_up(pdata);
	if (ret) {
		MCHP_WARN(pdata, drv, "Failed to power-up the PHY chip");
		return ret;
	}

	/* Reset the LAN9352 */
	mchp9352_reg_write(pdata, RESET_CTL, RESET_CTL_DIGITAL_RST_);

	if (mchp9352_wait_till_ready(pdata) != 0) {
		MCHP_WARN(pdata, drv, "device not ready");
		return -EIO;
	}

	/* verify reset bit is cleared */
	timeout = 10;
	do {
		usleep_range(10, 20);
		temp = mchp9352_reg_read(pdata, RESET_CTL);
	} while ((--timeout) && (temp & RESET_CTL_DIGITAL_RST_));

	if (unlikely(temp & RESET_CTL_DIGITAL_RST_)) {
		MCHP_WARN(pdata, drv, "Failed to complete reset");
		return -EIO;
	}

	ret = mchp9352_phy_enable_energy_detect(pdata);

	if (ret) {
		MCHP_WARN(pdata, drv, "Failed to wakeup the PHY chip");
		return ret;
	}

	return 0;
}

/* Sets the device MAC address to dev_addr, called with mac_lock held */
static void
mchp9352_set_hw_mac_address(struct mchp9352_data *pdata, u8 dev_addr[6])
{
	u32 mac_high16 = (dev_addr[5] << 8) | dev_addr[4];
	u32 mac_low32 = (dev_addr[3] << 24) | (dev_addr[2] << 16) |
	    (dev_addr[1] << 8) | dev_addr[0];

	MCHP_ASSERT_MAC_LOCK(pdata);

	mchp9352_mac_write(pdata, ADDRH, mac_high16);
	mchp9352_mac_write(pdata, ADDRL, mac_low32);
}

static void mchp9352_disable_irq_chip(struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);

	mchp9352_reg_write(pdata, INT_EN, 0);
	mchp9352_reg_write(pdata, INT_STS, 0xFFFFFFFF);
}

static int mchp9352_eeprom_is_busy(struct mchp9352_data *pdata)
{
	int result = 0;

	if (pdata) {
		if (mchp9352_reg_read(pdata, E2P_CMD) & E2P_CMD_EPC_BUSY_)
			result = 1;
	}
	return result;
}

static void mchp9352_eeprom_wait_till_done(struct mchp9352_data *pdata)
{
	unsigned int timeout = 50;

	while ((mchp9352_eeprom_is_busy(pdata)) && (--timeout))
		usleep_range(10, 20);

	if (unlikely(timeout == 0)) {
		MCHP_WARN(pdata, hw,
			  "Timed out waiting for EEPROM busy bit to clear");
	}
}

static int mchp9352_open(struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);
	unsigned int timeout;
	unsigned int temp;
	unsigned int intcfg;

	/* if the phy is not yet registered, retry later*/
	if (!pdata->phy_dev) {
		MCHP_WARN(pdata, hw, "phy_dev is NULL");
		return -EAGAIN;
	}

	/* Reset the LAN911x */
	if (mchp9352_soft_reset(pdata)) {
		MCHP_WARN(pdata, hw, "soft reset failed");
		return -EIO;
	}

	mchp9352_reg_write(pdata, HW_CFG,
			   HW_CFG_TX_FIF_SZ_(5) |
			   HW_CFG_SF_ /* on many parts this must be set */
			   );
	mchp9352_reg_write(pdata, AFC_CFG, 0x006E3740);

	/* Increase the legal frame size of VLAN tagged frames to 1522 bytes
	 */
	spin_lock_irq(&pdata->mac_lock);
	mchp9352_mac_write(pdata, VLAN1, ETH_P_8021Q);
	spin_unlock_irq(&pdata->mac_lock);

	/* Make sure EEPROM has finished loading before setting GPIO_CFG */
	mchp9352_eeprom_wait_till_done(pdata);

	/* configure as gpio */
	mchp9352_reg_write(pdata, LED_CFG, LED_CFG_ENABLE_(0xFF));

	/* disable 1588 and set to open drain */
	mchp9352_reg_write(pdata, GPIO_CFG, 0UL);

	/* set gpio's as input */
	mchp9352_reg_write(pdata, GPIO_DATA_DIR, 0UL);

	/* The soft reset above cleared the device's MAC address,
	 * restore it from local copy (set in probe)
	 */
	spin_lock_irq(&pdata->mac_lock);
	mchp9352_set_hw_mac_address(pdata, dev->dev_addr);
	spin_unlock_irq(&pdata->mac_lock);

	/* Initialise irqs, but leave all sources disabled */
	mchp9352_disable_irq_chip(dev);

	/* Set interrupt deassertion to 100uS */
	intcfg = ((10 << 24) | INT_CFG_IRQ_EN_);

	if (pdata->config.irq_polarity) {
		MCHP_TRACE(pdata, ifup, "irq polarity: active high");
		intcfg |= INT_CFG_IRQ_POL_;
	} else {
		MCHP_TRACE(pdata, ifup, "irq polarity: active low");
	}

	if (pdata->config.irq_type) {
		MCHP_TRACE(pdata, ifup, "irq type: push-pull");
		intcfg |= INT_CFG_IRQ_TYPE_;
	} else {
		MCHP_TRACE(pdata, ifup, "irq type: open drain");
	}

	mchp9352_reg_write(pdata, INT_CFG, intcfg);

	MCHP_TRACE(pdata, ifup, "Testing irq handler using IRQ %d", dev->irq);
	pdata->software_irq_signal = 0;

	/* Testing irq handler */
	smp_wmb();

	temp = mchp9352_reg_read(pdata, INT_EN);
	temp |= INT_EN_SW_INT_EN_;
	mchp9352_reg_write(pdata, INT_EN, temp);

	timeout = 1000;
	while (timeout--) {
		if (pdata->software_irq_signal)
			break;
		usleep_range(1000, 10000);
	}

	if (!pdata->software_irq_signal) {
		netdev_warn(dev, "ISR failed signaling test (IRQ %d)\n",
			    dev->irq);
		return -ENODEV;
	}
	MCHP_TRACE(pdata, ifup, "IRQ handler passed test using IRQ %d",
		   dev->irq);

	netdev_info(dev, "MCHP9352 identified at %#08lx, IRQ: %d\n",
		    (unsigned long)pdata->ioaddr, dev->irq);

	/* Reset the last known duplex and carrier */
	pdata->last_duplex = -1;
	pdata->last_carrier = -1;

	/* Bring the PHY up */
	phy_start(pdata->phy_dev);

	temp = mchp9352_reg_read(pdata, HW_CFG);
	/* Preserve TX FIFO size and external PHY configuration */
	temp &= (HW_CFG_TX_FIF_SZ_(0xFFFFFFFFUL) | 0x00000FFF);
	temp |= HW_CFG_SF_; /* on many parts this must be set */
	mchp9352_reg_write(pdata, HW_CFG, temp);

	temp = mchp9352_reg_read(pdata, FIFO_INT);
	temp |= FIFO_INT_TX_AVAIL_LEVEL_;
	temp &= ~(FIFO_INT_RX_STS_LEVEL_);
	mchp9352_reg_write(pdata, FIFO_INT, temp);

	/* set RX Data offset to 2 bytes for alignment */
	mchp9352_reg_write(pdata, RX_CFG, (NET_IP_ALIGN << 8));

	/* enable NAPI polling before enabling RX interrupts */
	napi_enable(&pdata->napi);

	temp = mchp9352_reg_read(pdata, INT_EN);
	temp |= (INT_EN_TDFA_EN_ | INT_EN_RSFL_EN_ | INT_EN_RXSTOP_INT_EN_);
	mchp9352_reg_write(pdata, INT_EN, temp);

	spin_lock_irq(&pdata->mac_lock);
	temp = mchp9352_mac_read(pdata, MAC_CR);
	temp |= (MAC_CR_TXEN_ | MAC_CR_RXEN_ | MAC_CR_HBDIS_);
	mchp9352_mac_write(pdata, MAC_CR, temp);
	spin_unlock_irq(&pdata->mac_lock);

	mchp9352_reg_write(pdata, TX_CFG, TX_CFG_TX_ON_);

	netif_start_queue(dev);
	return 0;
}

/* Entry point for stopping the interface */
static int mchp9352_stop(struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);
	unsigned int temp;

	/* Disable all device interrupts */
	temp = mchp9352_reg_read(pdata, INT_CFG);
	temp &= ~INT_CFG_IRQ_EN_;
	mchp9352_reg_write(pdata, INT_CFG, temp);

	/* Stop Tx and Rx polling */
	netif_stop_queue(dev);
	napi_disable(&pdata->napi);

	/* At this point all Rx and Tx activity is stopped */
	dev->stats.rx_dropped += mchp9352_reg_read(pdata, RX_DROP);
	mchp9352_tx_update_txcounters(dev);

	/* Bring the PHY down */
	if (pdata->phy_dev)
		phy_stop(pdata->phy_dev);

	MCHP_TRACE(pdata, ifdown, "Interface stopped");
	return 0;
}

/* Entry point for transmitting a packet */
static int mchp9352_hard_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);
	unsigned int freespace;
	unsigned int tx_cmd_a;
	unsigned int tx_cmd_b;
	unsigned int temp;
	u32 wrsz;
	ulong bufp;

	freespace = mchp9352_reg_read(pdata, TX_FIFO_INF) & TX_FIFO_INF_TDFREE_;

	if (unlikely(freespace < TX_FIFO_LOW_THRESHOLD))
		MCHP_WARN(pdata, tx_err,
			  "Tx data fifo low, space available: %d", freespace);

	/* Word alignment adjustment */
	tx_cmd_a = (u32)((ulong)skb->data & 0x03) << 16;
	tx_cmd_a |= TX_CMD_A_FIRST_SEG_ | TX_CMD_A_LAST_SEG_;
	tx_cmd_a |= (unsigned int)skb->len;

	tx_cmd_b = ((unsigned int)skb->len) << 16;
	tx_cmd_b |= (unsigned int)skb->len;

	mchp9352_reg_write(pdata, TX_DATA_FIFO, tx_cmd_a);
	mchp9352_reg_write(pdata, TX_DATA_FIFO, tx_cmd_b);

	bufp = (ulong)skb->data & (~0x3);
	wrsz = (u32)skb->len + 3;
	wrsz += (u32)((ulong)skb->data & 0x3);
	wrsz >>= 2;

	pdata->ops->tx_writefifo(pdata, (unsigned int *)bufp, wrsz);
	freespace -= (skb->len + 32);
	skb_tx_timestamp(skb);
	dev_consume_skb_any(skb);

	if (unlikely(mchp9352_tx_get_txstatcount(pdata) >= 30))
		mchp9352_tx_update_txcounters(dev);

	if (freespace < TX_FIFO_LOW_THRESHOLD) {
		netif_stop_queue(dev);
		temp = mchp9352_reg_read(pdata, FIFO_INT);
		temp &= 0x00FFFFFF;
		temp |= 0x32000000;
		mchp9352_reg_write(pdata, FIFO_INT, temp);
	}

	return NETDEV_TX_OK;
}

/* Entry point for getting status counters */
static struct net_device_stats *mchp9352_get_stats(struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);

	mchp9352_tx_update_txcounters(dev);
	dev->stats.rx_dropped += mchp9352_reg_read(pdata, RX_DROP);
	return &dev->stats;
}

/* Entry point for setting addressing modes */
static void mchp9352_set_multicast_list(struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);
	unsigned long flags;

	if (dev->flags & IFF_PROMISC) {
		/* Enabling promiscuous mode */
		pdata->set_bits_mask = MAC_CR_PRMS_;
		pdata->clear_bits_mask = (MAC_CR_MCPAS_ | MAC_CR_HPFILT_);
		pdata->hashhi = 0;
		pdata->hashlo = 0;
	} else if (dev->flags & IFF_ALLMULTI) {
		/* Enabling all multicast mode */
		pdata->set_bits_mask = MAC_CR_MCPAS_;
		pdata->clear_bits_mask = (MAC_CR_PRMS_ | MAC_CR_HPFILT_);
		pdata->hashhi = 0;
		pdata->hashlo = 0;
	} else if (!netdev_mc_empty(dev)) {
		/* Enabling specific multicast addresses */
		unsigned int hash_high = 0;
		unsigned int hash_low = 0;
		struct netdev_hw_addr *ha;

		pdata->set_bits_mask = MAC_CR_HPFILT_;
		pdata->clear_bits_mask = (MAC_CR_PRMS_ | MAC_CR_MCPAS_);

		netdev_for_each_mc_addr(ha, dev) {
			unsigned int bitnum = mchp9352_hash(ha->addr);
			unsigned int mask = 0x01 << (bitnum & 0x1F);

			if (bitnum & 0x20)
				hash_high |= mask;
			else
				hash_low |= mask;
		}

		pdata->hashhi = hash_high;
		pdata->hashlo = hash_low;
	} else {
		/* Enabling local MAC address only */
		pdata->set_bits_mask = 0;
		pdata->clear_bits_mask =
		    (MAC_CR_PRMS_ | MAC_CR_MCPAS_ | MAC_CR_HPFILT_);
		pdata->hashhi = 0;
		pdata->hashlo = 0;
	}

	spin_lock_irqsave(&pdata->mac_lock, flags);

	/* Newer hardware revision - can write immediately */
	mchp9352_rx_multicast_update(pdata);

	spin_unlock_irqrestore(&pdata->mac_lock, flags);
}

static irqreturn_t mchp9352_irqhandler(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct mchp9352_data *pdata = netdev_priv(dev);
	u32 intsts = mchp9352_reg_read(pdata, INT_STS);
	u32 inten = mchp9352_reg_read(pdata, INT_EN);
	int serviced = IRQ_NONE;
	u32 temp;

	if (unlikely(intsts & inten & INT_STS_SW_INT_)) {
		temp = mchp9352_reg_read(pdata, INT_EN);
		temp &= (~INT_EN_SW_INT_EN_);
		mchp9352_reg_write(pdata, INT_EN, temp);
		mchp9352_reg_write(pdata, INT_STS, INT_STS_SW_INT_);
		pdata->software_irq_signal = 1;
		smp_wmb();/* IRQ Handled */
		serviced = IRQ_HANDLED;
	}

	if (unlikely(intsts & inten & INT_STS_RXSTOP_INT_)) {
		/* Called when there is a multicast update scheduled and
		 * it is now safe to complete the update
		 */
		MCHP_TRACE(pdata, intr, "RX Stop interrupt");
		mchp9352_reg_write(pdata, INT_STS, INT_STS_RXSTOP_INT_);
		if (pdata->multicast_update_pending)
			mchp9352_rx_multicast_update_workaround(pdata);
		serviced = IRQ_HANDLED;
	}

	if (intsts & inten & INT_STS_TDFA_) {
		temp = mchp9352_reg_read(pdata, FIFO_INT);
		temp |= FIFO_INT_TX_AVAIL_LEVEL_;
		mchp9352_reg_write(pdata, FIFO_INT, temp);
		mchp9352_reg_write(pdata, INT_STS, INT_STS_TDFA_);
		netif_wake_queue(dev);
		serviced = IRQ_HANDLED;
	}

	if (unlikely(intsts & inten & INT_STS_RXE_)) {
		MCHP_TRACE(pdata, intr, "RX Error interrupt");
		mchp9352_reg_write(pdata, INT_STS, INT_STS_RXE_);
		serviced = IRQ_HANDLED;
	}

	if (likely(intsts & inten & INT_STS_RSFL_)) {
		if (likely(napi_schedule_prep(&pdata->napi))) {
			/* Disable Rx interrupts */
			temp = mchp9352_reg_read(pdata, INT_EN);
			temp &= (~INT_EN_RSFL_EN_);
			mchp9352_reg_write(pdata, INT_EN, temp);
			/* Schedule a NAPI poll */
			__napi_schedule(&pdata->napi);
		} else {
			MCHP_WARN(pdata, rx_err, "napi_schedule_prep failed");
		}
		serviced = IRQ_HANDLED;
	}

	return serviced;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void mchp9352_poll_controller(struct net_device *dev)
{
	disable_irq(dev->irq);
	mchp9352_irqhandler(0, dev);
	enable_irq(dev->irq);
}
#endif				/* CONFIG_NET_POLL_CONTROLLER */

static int mchp9352_set_mac_address(struct net_device *dev, void *p)
{
	struct mchp9352_data *pdata = netdev_priv(dev);
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	ether_addr_copy(dev->dev_addr, addr->sa_data);

	spin_lock_irq(&pdata->mac_lock);
	mchp9352_set_hw_mac_address(pdata, dev->dev_addr);
	spin_unlock_irq(&pdata->mac_lock);

	netdev_info(dev, "MAC Address: %pM\n", dev->dev_addr);

	return 0;
}

/* Standard ioctls for mii-tool */
static int mchp9352_do_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct mchp9352_data *pdata = netdev_priv(dev);

	if (!netif_running(dev) || !pdata->phy_dev)
		return -EINVAL;

	return phy_mii_ioctl(pdata->phy_dev, ifr, cmd);
}

static int
mchp9352_ethtool_getsettings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct mchp9352_data *pdata = netdev_priv(dev);

	cmd->maxtxpkt = 1;
	cmd->maxrxpkt = 1;
	return phy_ethtool_gset(pdata->phy_dev, cmd);
}

static int
mchp9352_ethtool_setsettings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct mchp9352_data *pdata = netdev_priv(dev);

	return phy_ethtool_sset(pdata->phy_dev, cmd);
}

static void mchp9352_ethtool_getdrvinfo(struct net_device *dev,
					struct ethtool_drvinfo *info)
{
	strlcpy(info->driver, MCHP_CHIPNAME, sizeof(info->driver));
	strlcpy(info->version, MCHP_DRV_VERSION, sizeof(info->version));
	strlcpy(info->bus_info, dev_name(dev->dev.parent),
		sizeof(info->bus_info));
}

static int mchp9352_ethtool_nwayreset(struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);

	return phy_start_aneg(pdata->phy_dev);
}

static u32 mchp9352_ethtool_getmsglevel(struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);

	return pdata->msg_enable;
}

static void mchp9352_ethtool_setmsglevel(struct net_device *dev, u32 level)
{
	struct mchp9352_data *pdata = netdev_priv(dev);

	pdata->msg_enable = level;
}

static int mchp9352_ethtool_getregslen(struct net_device *dev)
{
	return (((LAN_REGISTER_EXTENT - ID_REV) / 4 + 1) +
		(WUCSR - MAC_CR) + 1 + 32) * sizeof(u32);
}

static void
mchp9352_ethtool_getregs(struct net_device *dev, struct ethtool_regs *regs,
			 void *buf)
{
	struct mchp9352_data *pdata = netdev_priv(dev);
	struct phy_device *phy_dev = pdata->phy_dev;
	unsigned long flags;
	unsigned int i;
	unsigned int j = 0;
	u32 *data = buf;

	regs->version = pdata->idrev;
	for (i = ID_REV; i <= LAN_REGISTER_EXTENT; i += (sizeof(u32)))
		data[j++] = mchp9352_reg_read(pdata, i);

	for (i = MAC_CR; i <= WUCSR; i++) {
		spin_lock_irqsave(&pdata->mac_lock, flags);
		data[j++] = mchp9352_mac_read(pdata, i);
		spin_unlock_irqrestore(&pdata->mac_lock, flags);
	}

	for (i = 0; i <= 31; i++)
		data[j++] = mchp9352_mii_read(phy_dev->mdio.bus,
					      phy_dev->mdio.addr, i);
}

static void mchp9352_eeprom_enable_access(struct mchp9352_data *pdata)
{
	/* for switch products the EEPROM is enabled by default */
	usleep_range(1000, 10000);
}

static int mchp9352_eeprom_send_cmd(struct mchp9352_data *pdata, u32 op)
{
	int timeout = 100;
	u32 e2cmd;

	MCHP_TRACE(pdata, drv, "op 0x%08x", op);
	if (mchp9352_eeprom_is_busy(pdata)) {
		MCHP_WARN(pdata, drv, "Busy at start");
		return -EBUSY;
	}

	e2cmd = op | E2P_CMD_EPC_BUSY_;
	mchp9352_reg_write(pdata, E2P_CMD, e2cmd);

	do {
		usleep_range(1000, 10000);
		e2cmd = mchp9352_reg_read(pdata, E2P_CMD);
	} while ((e2cmd & E2P_CMD_EPC_BUSY_) && (--timeout));

	if (!timeout) {
		MCHP_TRACE(pdata, drv, "TIMED OUT");
		return -EAGAIN;
	}

	if (e2cmd & E2P_CMD_EPC_TIMEOUT_) {
		/* note some parts don't support EWDS, EWEN, WRAL, ERASE, ERAL.
		 * So this timeout error will occur in those cases.
		 * But WRITE operations should still work.
		 * I will leave the following error message, incase it is
		 * reporting a real error. But if not, then just ignore the
		 * error and clear the timeout bit, so a following WRITE
		 * operation should work.
		 */
		MCHP_TRACE(pdata, drv,
			   "Possible error occurred during eeprom operation");
		/* clear the time out so that future operations will work */
		mchp9352_reg_write(pdata, E2P_CMD, E2P_CMD_EPC_TIMEOUT_);
		return -EINVAL;
	}

	return 0;
}

static int mchp9352_eeprom_read_location(struct mchp9352_data *pdata,
					 u8 address, u8 *data)
{
	u32 op = E2P_CMD_EPC_CMD_READ_ | address;
	int ret;

	MCHP_TRACE(pdata, drv, "address 0x%x", address);
	ret = mchp9352_eeprom_send_cmd(pdata, op);

	if (!ret)
		data[address] = mchp9352_reg_read(pdata, E2P_DATA);

	return ret;
}

static int mchp9352_eeprom_write_location(struct mchp9352_data *pdata,
					  u8 address, u8 data)
{
	u32 op = E2P_CMD_EPC_CMD_ERASE_ | address;
	u32 temp;
	int ret;

	MCHP_TRACE(pdata, drv, "address 0x%x, data 0x%x", address, data);
	ret = mchp9352_eeprom_send_cmd(pdata, op);
	if (ret) {
		/* some switch parts don't use the ERASE command,
		 * never the less the following WRITE command should work.
		 * Report this error incase it is causing a real problem.
		 * But allow the write to proceed incase it is not a problem
		 */
		MCHP_TRACE(pdata, drv, "ERROR in EEPROM ERASE command.");
		ret = 0;
	}

	if (!ret) {
		op = E2P_CMD_EPC_CMD_WRITE_ | address;
		mchp9352_reg_write(pdata, E2P_DATA, (u32)data);

		/* Workaround for hardware read-after-write restriction */
		temp = mchp9352_reg_read(pdata, BYTE_TEST);

		ret = mchp9352_eeprom_send_cmd(pdata, op);
	}

	return ret;
}

static int mchp9352_ethtool_get_eeprom_len(struct net_device *dev)
{
	return MCHP9352_EEPROM_SIZE;
}

static int mchp9352_ethtool_get_eeprom(struct net_device *dev,
				       struct ethtool_eeprom *eeprom, u8 *data)
{
	struct mchp9352_data *pdata = netdev_priv(dev);
	u8 eeprom_data[MCHP9352_EEPROM_SIZE];
	int len;
	int i;
	int start;
	int end;

	mchp9352_eeprom_enable_access(pdata);

	start = eeprom->offset;
	end = min((start + eeprom->len), MCHP9352_EEPROM_SIZE);
	len = end - start;
	for (i = start; i < end; i++) {
		int ret = mchp9352_eeprom_read_location(pdata, i, eeprom_data);

		if (ret < 0) {
			eeprom->len = 0;
			return ret;
		}
	}

	memcpy(data, &eeprom_data[start], len);
	eeprom->len = len;
	return 0;
}

static int mchp9352_ethtool_set_eeprom(struct net_device *dev,
				       struct ethtool_eeprom *eeprom, u8 *data)
{
	int ret;
	struct mchp9352_data *pdata = netdev_priv(dev);

	mchp9352_eeprom_enable_access(pdata);
	mchp9352_eeprom_send_cmd(pdata, E2P_CMD_EPC_CMD_EWEN_);
	ret = mchp9352_eeprom_write_location(pdata, eeprom->offset, *data);
	mchp9352_eeprom_send_cmd(pdata, E2P_CMD_EPC_CMD_EWDS_);

	/* Single byte write, according to man page */
	eeprom->len = 1;

	return ret;
}

static const struct ethtool_ops mchp9352_ethtool_ops = {
	.get_settings = mchp9352_ethtool_getsettings,
	.set_settings = mchp9352_ethtool_setsettings,
	.get_link = ethtool_op_get_link,
	.get_drvinfo = mchp9352_ethtool_getdrvinfo,
	.nway_reset = mchp9352_ethtool_nwayreset,
	.get_msglevel = mchp9352_ethtool_getmsglevel,
	.set_msglevel = mchp9352_ethtool_setmsglevel,
	.get_regs_len = mchp9352_ethtool_getregslen,
	.get_regs = mchp9352_ethtool_getregs,
	.get_eeprom_len = mchp9352_ethtool_get_eeprom_len,
	.get_eeprom = mchp9352_ethtool_get_eeprom,
	.set_eeprom = mchp9352_ethtool_set_eeprom,
	.get_ts_info = ethtool_op_get_ts_info,
};

static const struct net_device_ops mchp9352_netdev_ops = {
	.ndo_open		= mchp9352_open,
	.ndo_stop		= mchp9352_stop,
	.ndo_start_xmit		= mchp9352_hard_start_xmit,
	.ndo_get_stats		= mchp9352_get_stats,
	.ndo_set_rx_mode	= mchp9352_set_multicast_list,
	.ndo_do_ioctl		= mchp9352_do_ioctl,
	.ndo_change_mtu		= eth_change_mtu,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= mchp9352_set_mac_address,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= mchp9352_poll_controller,
#endif
};

/* copies the current mac address from hardware to dev->dev_addr */
static void mchp9352_read_mac_address(struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);
	u32 mac_high16 = mchp9352_mac_read(pdata, ADDRH);
	u32 mac_low32 = mchp9352_mac_read(pdata, ADDRL);

	dev->dev_addr[0] = (u8)(mac_low32);
	dev->dev_addr[1] = (u8)(mac_low32 >> 8);
	dev->dev_addr[2] = (u8)(mac_low32 >> 16);
	dev->dev_addr[3] = (u8)(mac_low32 >> 24);
	dev->dev_addr[4] = (u8)(mac_high16);
	dev->dev_addr[5] = (u8)(mac_high16 >> 8);
}

/* To be called before any other access, and after reset */
static int mchp9352_wait_till_ready(struct mchp9352_data *pdata)
{
	/* BYTE_TEST must read correctly before
	 * any other register read is legitimate
	 */
	unsigned byte_test = 0;
	unsigned count = 0;

	byte_test = mchp9352_reg_read(pdata, BYTE_TEST);
	while ((byte_test != 0x87654321) && (byte_test != 0x43218765) &&
	       (count < 1000)) {
		usleep_range(100, 200);
		byte_test = mchp9352_reg_read(pdata, BYTE_TEST);
		count++;
	}

	if (byte_test == 0x43218765) {
		MCHP_TRACE(pdata, probe,
			   "BYTE_TEST looks swapped, applying WORD_SWAP");
		mchp9352_reg_write(pdata, WORD_SWAP, 0xffffffff);

		/* 1 dummy read of BYTE_TEST is needed after a write to
		 * WORD_SWAP before its contents are valid
		 */
		byte_test = mchp9352_reg_read(pdata, BYTE_TEST);

		byte_test = mchp9352_reg_read(pdata, BYTE_TEST);
	}

	if (byte_test != 0x87654321) {
		MCHP_WARN(pdata, drv, "BYTE_TEST: 0x%08X", byte_test);
		if (((byte_test >> 16) & 0xFFFF) == (byte_test & 0xFFFF)) {
			MCHP_WARN(pdata, probe,
				  "top 16 bits equal to bottom 16 bits");
			MCHP_TRACE(pdata, probe,
				   "This may mean the chip is set for 32 bit while the bus is reading 16 bit");
		}
		return -ENODEV;
	}
	return 0;
}

/* Initializing private device structures, only called from probe */
static int mchp9352_init(struct net_device *dev)
{
	struct mchp9352_data *pdata = netdev_priv(dev);
	unsigned int byte_test, mask;
	unsigned int to = 100;

	MCHP_TRACE(pdata, probe, "Driver Parameters:");
	MCHP_TRACE(pdata, probe, "LAN base: 0x%08lX",
		   (unsigned long)pdata->ioaddr);
	MCHP_TRACE(pdata, probe, "IRQ: %d", dev->irq);
	MCHP_TRACE(pdata, probe, "PHY will be autodetected.");

	spin_lock_init(&pdata->dev_lock);
	spin_lock_init(&pdata->mac_lock);

	if (!pdata->ioaddr) {
		MCHP_WARN(pdata, probe, "pdata->ioaddr: 0x00000000");
		return -ENODEV;
	}

	if (mchp9352_wait_till_ready(pdata) != 0) {
		MCHP_WARN(pdata, probe, "failed mchp9352_wait_till_ready");
		return -ENODEV;
	}

	/* poll the READY bit in PMT_CTRL. Any other access to the device is
	 * forbidden while this bit isn't set. Try for 100ms
	 *
	 * Note that this test is done before the WORD_SWAP register is
	 * programmed. So in some configurations the READY bit is at 16 before
	 * WORD_SWAP is written to. This issue is worked around by waiting
	 * until either bit 0 or bit 16 gets set in PMT_CTRL.
	 *
	 * SMSC has confirmed that checking bit 16 (marked as reserved in
	 * the datasheet) is fine since these bits "will either never be set
	 * or can only go high after READY does (so also indicate the device
	 * is ready)".
	 */

	mask = PMT_CTRL_READY_ | swahw32(PMT_CTRL_READY_);
	while (!(mchp9352_reg_read(pdata, PMT_CTRL) & mask) && --to)
		usleep_range(1000, 2000);

	if (to == 0) {
		netdev_err(dev, "Device not READY in 100ms aborting\n");
		return -ENODEV;
	}

	/* Check byte ordering */
	byte_test = mchp9352_reg_read(pdata, BYTE_TEST);
	MCHP_TRACE(pdata, probe, "BYTE_TEST: 0x%08X", byte_test);
	if (byte_test == 0x43218765) {
		MCHP_TRACE(pdata, probe,
			   "BYTE_TEST looks swapped, applying WORD_SWAP");
		mchp9352_reg_write(pdata, WORD_SWAP, 0xffffffff);

		/* 1 dummy read of BYTE_TEST is needed after a write to
		 * WORD_SWAP before its contents are valid
		 */
		byte_test = mchp9352_reg_read(pdata, BYTE_TEST);

		byte_test = mchp9352_reg_read(pdata, BYTE_TEST);
	}

	if (byte_test != 0x87654321) {
		MCHP_WARN(pdata, drv, "BYTE_TEST: 0x%08X", byte_test);
		if (((byte_test >> 16) & 0xFFFF) == (byte_test & 0xFFFF)) {
			MCHP_WARN(pdata, probe,
				  "top 16 bits equal to bottom 16 bits");
			MCHP_TRACE(pdata, probe,
				   "This may mean the chip is set for 32 bit while the bus is reading 16 bit");
		}
		return -ENODEV;
	}

	pdata->idrev = mchp9352_reg_read(pdata, ID_REV);
	switch (pdata->idrev & 0xFFFF0000) {
	case 0x92500000:
	case 0x93110000:
	case 0x93120000:
	case 0x93520000:
		/* LAN9250/LAN9311/LAN9312/LAN9352 */
		break;

	default:
		MCHP_WARN(pdata, probe, "Switch not identified, idrev: 0x%08X",
			  pdata->idrev);
		return -ENODEV;
	}

	MCHP_TRACE(pdata, probe,
		   "Switch identified, idrev: 0x%08X", pdata->idrev);

	/* workaround for platforms without an eeprom, where the mac address
	 * is stored elsewhere and set by the bootloader.  This saves the
	 * mac address before resetting the device
	 */
	if (pdata->config.flags & MCHP9352_SAVE_MAC_ADDRESS) {
		spin_lock_irq(&pdata->mac_lock);
		mchp9352_read_mac_address(dev);
		spin_unlock_irq(&pdata->mac_lock);
	}

	/* Reset the LAN9352 */
	if (mchp9352_phy_reset(pdata) || mchp9352_soft_reset(pdata))
		return -ENODEV;

	dev->flags |= IFF_MULTICAST;
	netif_napi_add(dev, &pdata->napi, mchp9352_poll, MCHP_NAPI_WEIGHT);
	dev->netdev_ops = &mchp9352_netdev_ops;
	dev->ethtool_ops = &mchp9352_ethtool_ops;

	return 0;
}

static int mchp9352_drv_remove(struct platform_device *pdev)
{
	struct net_device *dev;
	struct mchp9352_data *pdata;
	struct resource *res;

	dev = platform_get_drvdata(pdev);
	WARN_ON(!dev);
	pdata = netdev_priv(dev);
	WARN_ON(!pdata);
	WARN_ON(!pdata->ioaddr);
	WARN_ON(!pdata->phy_dev);

	MCHP_TRACE(pdata, ifdown, "Stopping driver");

	phy_disconnect(pdata->phy_dev);
	pdata->phy_dev = NULL;
	mdiobus_unregister(pdata->mii_bus);
	mdiobus_free(pdata->mii_bus);

	unregister_netdev(dev);
	free_irq(dev->irq, dev);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "mchp9352-memory");
	if (!res)
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	release_mem_region(res->start, resource_size(res));

	iounmap(pdata->ioaddr);

	(void)mchp9352_disable_resources(pdev);
	mchp9352_free_resources(pdev);

	free_netdev(dev);

	pm_runtime_put(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

/* standard register acces */
static const struct mchp9352_ops standard_mchp9352_ops = {
	.reg_read = __mchp9352_reg_read,
	.reg_write = __mchp9352_reg_write,
	.rx_readfifo = mchp9352_rx_readfifo,
	.tx_writefifo = mchp9352_tx_writefifo,
};

/* shifted register access */
static const struct mchp9352_ops shifted_mchp9352_ops = {
	.reg_read = __mchp9352_reg_read_shift,
	.reg_write = __mchp9352_reg_write_shift,
	.rx_readfifo = mchp9352_rx_readfifo_shift,
	.tx_writefifo = mchp9352_tx_writefifo_shift,
};

static int mchp9352_probe_config(struct mchp9352_platform_config *config,
				 struct device *dev)
{
	int phy_interface;
	u32 width = 0;
	int err;

	phy_interface = device_get_phy_mode(dev);
	if (phy_interface < 0)
		phy_interface = PHY_INTERFACE_MODE_NA;
	config->phy_interface = phy_interface;

	device_get_mac_address(dev, config->mac, ETH_ALEN);

	err = device_property_read_u32(dev, "reg-io-width", &width);
	if (err == -ENXIO)
		return err;
	if (!err && width == 4)
		config->flags |= MCHP9352_USE_32BIT;
	else
		config->flags |= MCHP9352_USE_16BIT;

	device_property_read_u32(dev, "reg-shift", &config->shift);

	if (device_property_present(dev, "microchip,irq-active-high"))
		config->irq_polarity = MCHP9352_IRQ_POLARITY_ACTIVE_HIGH;

	if (device_property_present(dev, "microchip,irq-push-pull"))
		config->irq_type = MCHP9352_IRQ_TYPE_PUSH_PULL;

	if (device_property_present(dev, "microchip,save-mac-address"))
		config->flags |= MCHP9352_SAVE_MAC_ADDRESS;

	return 0;
}

static int mchp9352_drv_probe(struct platform_device *pdev)
{
	struct net_device *dev;
	struct mchp9352_data *pdata;
	struct mchp9352_platform_config *config = dev_get_platdata(&pdev->dev);
	struct resource *res;
	unsigned int intcfg = 0;
	int res_size, irq, irq_flags;
	int retval;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "mchp9352-memory");
	if (!res)
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		pr_warn("Could not allocate resource\n");
		retval = -ENODEV;
		goto out_0;
	}
	res_size = resource_size(res);

	irq = platform_get_irq(pdev, 0);
	if (irq == -EPROBE_DEFER) {
		retval = -EPROBE_DEFER;
		goto out_0;
	} else if (irq <= 0) {
		pr_warn("Could not allocate irq resource\n");
		retval = -ENODEV;
		goto out_0;
	}

	if (!request_mem_region(res->start, res_size, MCHP_CHIPNAME)) {
		retval = -EBUSY;
		goto out_0;
	}

	dev = alloc_etherdev(sizeof(struct mchp9352_data));
	if (!dev) {
		retval = -ENOMEM;
		goto out_release_io_1;
	}

	SET_NETDEV_DEV(dev, &pdev->dev);

	pdata = netdev_priv(dev);
	dev->irq = irq;
	irq_flags = irq_get_trigger_type(irq);
	pdata->ioaddr = ioremap_nocache(res->start, res_size);

	pdata->dev = dev;
	pdata->msg_enable = ((1 << debug) - 1);

	platform_set_drvdata(pdev, dev);

	retval = mchp9352_request_resources(pdev);
	if (retval)
		goto out_request_resources_fail;

	retval = mchp9352_enable_resources(pdev);
	if (retval)
		goto out_enable_resources_fail;

	if (!pdata->ioaddr) {
		MCHP_WARN(pdata, probe, "Error mchp9352 base address invalid");
		retval = -ENOMEM;
		goto out_disable_resources;
	}

	retval = mchp9352_probe_config(&pdata->config, &pdev->dev);
	if (retval && config) {
		/* copy config parameters across to pdata */
		memcpy(&pdata->config, config, sizeof(pdata->config));
		retval = 0;
	}

	if (retval) {
		MCHP_WARN(pdata, probe, "Error mchp9352 config not found");
		goto out_disable_resources;
	}

	/* assume standard, non-shifted, access to HW registers */
	pdata->ops = &standard_mchp9352_ops;
	/* apply the right access if shifting is needed */
	if (pdata->config.shift)
		pdata->ops = &shifted_mchp9352_ops;

	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);

	retval = mchp9352_init(dev);
	if (retval < 0)
		goto out_disable_resources;

	/* configure irq polarity and type before connecting isr */
	if (pdata->config.irq_polarity == MCHP9352_IRQ_POLARITY_ACTIVE_HIGH)
		intcfg |= INT_CFG_IRQ_POL_;

	if (pdata->config.irq_type == MCHP9352_IRQ_TYPE_PUSH_PULL)
		intcfg |= INT_CFG_IRQ_TYPE_;

	if (mchp9352_wait_till_ready(pdata) != 0)
		goto out_disable_resources;

	mchp9352_reg_write(pdata, INT_CFG, intcfg);

	/* Ensure interrupts are globally disabled before connecting ISR */
	mchp9352_disable_irq_chip(dev);

	retval = request_irq(dev->irq, mchp9352_irqhandler,
			     irq_flags | IRQF_SHARED, dev->name, dev);
	if (retval) {
		MCHP_WARN(pdata, probe,
			  "Unable to claim requested irq: %d", dev->irq);
		goto out_disable_resources;
	}

	netif_carrier_off(dev);

	retval = register_netdev(dev);
	if (retval) {
		MCHP_WARN(pdata, probe, "Error %i registering device", retval);
		goto out_free_irq;
	} else {
		MCHP_TRACE(pdata, probe,
			   "Network interface: \"%s\"", dev->name);
	}

	retval = mchp9352_mii_init(pdev, dev);
	if (retval) {
		MCHP_WARN(pdata, probe, "Error %i initialising mii", retval);
		goto out_unregister_netdev_5;
	}

	spin_lock_irq(&pdata->mac_lock);

	/* Check if mac address has been specified when bringing interface up */
	if (is_valid_ether_addr(dev->dev_addr)) {
		mchp9352_set_hw_mac_address(pdata, dev->dev_addr);
		MCHP_TRACE(pdata, probe,
			   "MAC Address is specified by configuration");
	} else if (is_valid_ether_addr(pdata->config.mac)) {
		ether_addr_copy(dev->dev_addr, pdata->config.mac);
		MCHP_TRACE(pdata, probe,
			   "MAC Address specified by platform data");
	} else {
		/* Try reading mac address from device. if EEPROM is present
		 * it will already have been set
		 */
		mchp_get_mac(dev);

		if (is_valid_ether_addr(dev->dev_addr)) {
			/* eeprom values are valid  so use them */
			MCHP_TRACE(pdata, probe,
				   "Mac Address is read from LAN9352 EEPROM");
		} else {
			/* eeprom values are invalid, generate random MAC */
			eth_hw_addr_random(dev);
			mchp9352_set_hw_mac_address(pdata, dev->dev_addr);
			MCHP_TRACE(pdata, probe,
				   "MAC Address is set to eth_random_addr");
		}
	}

	spin_unlock_irq(&pdata->mac_lock);

	netdev_info(dev, "MAC Address: %pM\n", dev->dev_addr);
	return 0;

out_unregister_netdev_5:
	unregister_netdev(dev);
out_free_irq:
	free_irq(dev->irq, dev);
out_disable_resources:
	pm_runtime_put(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	(void)mchp9352_disable_resources(pdev);
out_enable_resources_fail:
	mchp9352_free_resources(pdev);
out_request_resources_fail:
	iounmap(pdata->ioaddr);
	free_netdev(dev);
out_release_io_1:
	release_mem_region(res->start, resource_size(res));
out_0:

	return retval;
}

#ifdef CONFIG_PM
/* This implementation assumes the devices remains powered on its VDDVARIO
 * pins during suspend.
 */

/* TODO: implement freeze/thaw callbacks for hibernation.*/

static int mchp9352_suspend(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct mchp9352_data *pdata = netdev_priv(ndev);

	/* enable wake on LAN, energy detection and the external PME
	 * signal.
	 */
	mchp9352_reg_write(pdata, PMT_CTRL,
			   PMT_CTRL_PM_MODE_D1_ | PMT_CTRL_WOL_EN_ |
			   PMT_CTRL_ED_EN_ | PMT_CTRL_PME_EN_);

	return 0;
}

static int mchp9352_resume(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct mchp9352_data *pdata = netdev_priv(ndev);
	unsigned int to = 100;

	/* Writing any data to the BYTE_TEST register will wake-up the device.
	 */
	mchp9352_reg_write(pdata, BYTE_TEST, 0);

	/* poll the READY bit in PMT_CTRL. Any other access to the device is
	 * forbidden while this bit isn't set. Try for 100ms and return -EIO
	 * if it failed.
	 */
	while (!(mchp9352_reg_read(pdata, PMT_CTRL) & PMT_CTRL_READY_) && --to)
		usleep_range(1000, 2000);

	return (to == 0) ? -EIO : 0;
}

static const struct dev_pm_ops mchp9352_pm_ops = {
	.suspend	= mchp9352_suspend,
	.resume		= mchp9352_resume,
};

#define MCHP9352_PM_OPS (&mchp9352_pm_ops)

#else
#define MCHP9352_PM_OPS NULL
#endif

#ifdef CONFIG_OF
static const struct of_device_id mchp9352_dt_ids[] = {
	{ .compatible = "microchip,lan9352", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mchp9352_dt_ids);
#endif

static const struct acpi_device_id mchp9352_acpi_match[] = {
	{ "ARMH9352", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, mchp9352_acpi_match);

static struct platform_driver mchp9352_driver = {
	.probe = mchp9352_drv_probe,
	.remove = mchp9352_drv_remove,
	.driver = {
		.name	= MCHP_CHIPNAME,
		.pm	= MCHP9352_PM_OPS,
		.of_match_table = of_match_ptr(mchp9352_dt_ids),
		.acpi_match_table = ACPI_PTR(mchp9352_acpi_match),
	},
};

/* Entry point for loading the module */
static int __init mchp9352_init_module(void)
{
	MCHP_INITIALIZE();
	return platform_driver_register(&mchp9352_driver);
}

/* entry point for unloading the module */
static void __exit mchp9352_cleanup_module(void)
{
	platform_driver_unregister(&mchp9352_driver);
}

module_init(mchp9352_init_module);
module_exit(mchp9352_cleanup_module);
