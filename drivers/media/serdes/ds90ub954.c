// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2019 Luca Ceresoli <luca@lucaceresoli.net>

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/i2c-mux.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <media/v4l2-subdev.h>

#define DS90_NUM_RXPORTS		2  /* Physical RX ports */

#define DS90_NUM_GPIOS			7  /* Physical GPIO pins */
#define DS90_NUM_BC_GPIOS		4  /* Max GPIOs on Back Channel */
#define DS90_GPIO_NSOURCES		8  /* Possible GPIO out sources */
#define DS90_GPIO_NFUNCS		8  /* Possible GPIO out functions */

#define DS90_NUM_SLAVE_ALIASES		8
#define DS90_MAX_POOL_ALIASES		(DS90_NUM_RXPORTS * DS90_NUM_SLAVE_ALIASES)

#define DS90_V4L2_NUM_PADS		2  /* TODO +1 for DS90_NUM_CSITXPORTS */

#define DS90_REG_I2C_DEV_ID		0x00
#define DS90_REG_RESET			0x01
#define DS90_REG_GEN_CONFIG		0x02
#define DS90_REG_REV_MASK		0x03
#define DS90_REG_DEVICE_STS		0x04
#define DS90_REG_PAR_ERR_THOLD_HI	0x05
#define DS90_REG_PAR_ERR_THOLD_LO	0x06
#define DS90_REG_BCC_WDOG_CTL		0x07
#define DS90_REG_I2C_CTL1		0x08
#define DS90_REG_I2C_CTL2		0x09
#define DS90_REG_SCL_HIGH_TIME		0x0A
#define DS90_REG_SCL_LOW_TIME		0x0B
#define DS90_REG_RX_PORT_CTL		0x0C
#define DS90_REG_IO_CTL			0x0D
#define DS90_REG_GPIO_PIN_STS		0x0E
#define DS90_REG_GPIO_INPUT_CTL		0x0F
#define DS90_REG_GPIO_PIN_CTL(n)	(0x10 + (n)) /* n < DS90_NUM_GPIOS */
#define DS90_REG_FS_CTL			0x18
#define DS90_REG_FS_HIGH_TIME_1		0x19
#define DS90_REG_FS_HIGH_TIME_0		0x1A
#define DS90_REG_FS_LOW_TIME_1		0x1B
#define DS90_REG_FS_LOW_TIME_0		0x1C
#define DS90_REG_MAX_FRM_HI		0x1D
#define DS90_REG_MAX_FRM_LO		0x1E
#define DS90_REG_CSI_PLL_CTL		0x1F
#define DS90_REG_FWD_CTL1		0x20
#define DS90_REG_FWD_CTL2		0x21
#define DS90_REG_FWD_STS		0x22

#define DS90_REG_INTERRUPT_CTL		0x23
#define DS90_REG_INTERRUPT_CTL_INT_EN		BIT(7)
#define DS90_REG_INTERRUPT_CTL_IE_CSI_TX0	BIT(4)
#define DS90_REG_INTERRUPT_CTL_IE_RX(n)		BIT((n)) /* rxport[n] IRQ */

#define DS90_REG_INTERRUPT_STS		0x24
#define DS90_REG_INTERRUPT_STS_INT		BIT(7)
#define DS90_REG_INTERRUPT_STS_IS_CSI_TX0	BIT(4)
#define DS90_REG_INTERRUPT_STS_IS_RX(n)		BIT((n)) /* rxport[n] IRQ */

#define DS90_REG_TS_CONFIG		0x25
#define DS90_REG_TS_CONTROL		0x26
#define DS90_REG_TS_LINE_HI		0x27
#define DS90_REG_TS_LINE_LO		0x28
#define DS90_REG_TS_STATUS		0x29
#define DS90_REG_TIMESTAMP_P0_HI	0x2A
#define DS90_REG_TIMESTAMP_P0_LO	0x2B
#define DS90_REG_TIMESTAMP_P1_HI	0x2C
#define DS90_REG_TIMESTAMP_P1_LO	0x2D
#define DS90_REG_CSI_CTL		0x33
#define DS90_REG_CSI_CTL2		0x34
#define DS90_REG_CSI_STS		0x35
#define DS90_REG_CSI_TX_ICR		0x36
#define DS90_REG_CSI_TX_ISR		0x37
#define DS90_REG_CSI_TX_ISR_IS_CSI_SYNC_ERROR	BIT(3)
#define DS90_REG_CSI_TX_ISR_IS_CSI_PASS_ERROR	BIT(1)

#define DS90_REG_CSI_TEST_CTL		0x38
#define DS90_REG_CSI_TEST_PATT_HI	0x39
#define DS90_REG_CSI_TEST_PATT_LO	0x3A
#define DS90_REG_AEQ_CTL1		0x42
#define DS90_REG_AEQ_ERR_THOLD		0x43
#define DS90_REG_FPD3_CAP		0x4A
#define DS90_REG_RAW_EMBED_DTYPE	0x4B
#define DS90_REG_FPD3_PORT_SEL		0x4C

#define DS90_REG_RX_PORT_STS1		0x4D
#define DS90_REG_RX_PORT_STS1_BCC_CRC_ERROR	BIT(5)
#define DS90_REG_RX_PORT_STS1_LOCK_STS_CHG	BIT(4)
#define DS90_REG_RX_PORT_STS1_BCC_SEQ_ERROR	BIT(3)
#define DS90_REG_RX_PORT_STS1_PARITY_ERROR	BIT(2)
#define DS90_REG_RX_PORT_STS1_PORT_PASS		BIT(1)
#define DS90_REG_RX_PORT_STS1_LOCK_STS		BIT(0)

#define DS90_REG_RX_PORT_STS2		0x4E
#define DS90_REG_RX_PORT_STS2_LINE_LEN_UNSTABLE	BIT(7)
#define DS90_REG_RX_PORT_STS2_LINE_LEN_CHG	BIT(6)
#define DS90_REG_RX_PORT_STS2_FPD3_ENCODE_ERROR	BIT(5)
#define DS90_REG_RX_PORT_STS2_BUFFER_ERROR	BIT(4)
#define DS90_REG_RX_PORT_STS2_CSI_ERROR		BIT(3)
#define DS90_REG_RX_PORT_STS2_FREQ_STABLE	BIT(2)
#define DS90_REG_RX_PORT_STS2_CABLE_FAULT	BIT(1)
#define DS90_REG_RX_PORT_STS2_LINE_CNT_CHG	BIT(0)

#define DS90_REG_RX_FREQ_HIGH		0x4F
#define DS90_REG_RX_FREQ_LOW		0x50
#define DS90_REG_SENSOR_STS_0		0x51
#define DS90_REG_SENSOR_STS_1		0x52
#define DS90_REG_SENSOR_STS_2		0x53
#define DS90_REG_SENSOR_STS_3		0x54
#define DS90_REG_RX_PAR_ERR_HI		0x55
#define DS90_REG_RX_PAR_ERR_LO		0x56
#define DS90_REG_BIST_ERR_COUNT		0x57

#define DS90_REG_BCC_CONFIG		0x58
#define DS90_REG_BCC_CONFIG_I2C_PASS_THROUGH	BIT(6)

#define DS90_REG_DATAPATH_CTL1		0x59
#define DS90_REG_DATAPATH_CTL2		0x5A
#define DS90_REG_SER_ID			0x5B
#define DS90_REG_SER_ALIAS_ID		0x5C

#define DS90_REG_SLAVE_ID(n)		(0x5D + (n)) /* n < DS90_NUM_SLAVE_ALIASES */
#define DS90_REG_SLAVE_ALIAS(n)		(0x65 + (n)) /* n < DS90_NUM_SLAVE_ALIASES */

#define DS90_REG_PORT_CONFIG		0x6D
#define DS90_REG_BC_GPIO_CTL(n)		(0x6E + (n)) /* n < 2 */
#define DS90_REG_RAW10_ID		0x70
#define DS90_REG_RAW12_ID		0x71
#define DS90_REG_CSI_VC_MAP		0x72
#define DS90_REG_LINE_COUNT_HI		0x73
#define DS90_REG_LINE_COUNT_LO		0x74
#define DS90_REG_LINE_LEN_1		0x75
#define DS90_REG_LINE_LEN_0		0x76
#define DS90_REG_FREQ_DET_CTL		0x77
#define DS90_REG_MAILBOX_1		0x78
#define DS90_REG_MAILBOX_2		0x79

#define DS90_REG_CSI_RX_STS		0x7A
#define DS90_REG_CSI_RX_STS_LENGTH_ERR		BIT(3)
#define DS90_REG_CSI_RX_STS_CKSUM_ERR		BIT(2)
#define DS90_REG_CSI_RX_STS_ECC2_ERR		BIT(1)
#define DS90_REG_CSI_RX_STS_ECC1_ERR		BIT(0)

#define DS90_REG_CSI_ERR_COUNTER	0x7B
#define DS90_REG_PORT_CONFIG2		0x7C
#define DS90_REG_PORT_PASS_CTL		0x7D
#define DS90_REG_SEN_INT_RISE_CTL	0x7E
#define DS90_REG_SEN_INT_FALL_CTL	0x7F
#define DS90_REG_REFCLK_FREQ		0xA5
#define DS90_REG_IND_ACC_CTL		0xB0
#define DS90_REG_IND_ACC_ADDR		0xB1
#define DS90_REG_IND_ACC_DATA		0xB2
#define DS90_REG_BIST_CONTROL		0xB3
#define DS90_REG_MODE_IDX_STS		0xB8
#define DS90_REG_LINK_ERROR_COUNT	0xB9
#define DS90_REG_FPD3_ENC_CTL		0xBA
#define DS90_REG_FV_MIN_TIME		0xBC
#define DS90_REG_GPIO_PD_CTL		0xBE
#define DS90_REG_PORT_DEBUG		0xD0
#define DS90_REG_AEQ_CTL2		0xD2
#define DS90_REG_AEQ_STATUS		0xD3
#define DS90_REG_AEQ_BYPASS		0xD4
#define DS90_REG_AEQ_MIN_MAX		0xD5
#define DS90_REG_PORT_ICR_HI		0xD8
#define DS90_REG_PORT_ICR_LO		0xD9
#define DS90_REG_PORT_ISR_HI		0xDA
#define DS90_REG_PORT_ISR_LO		0xDB
#define DS90_REG_FC_GPIO_STS		0xDC
#define DS90_REG_FC_GPIO_ICR		0xDD
#define DS90_REG_SEN_INT_RISE_STS	0xDE
#define DS90_REG_SEN_INT_FALL_STS	0xDF
#define DS90_REG_FPD3_RX_ID0		0xF0
#define DS90_REG_FPD3_RX_ID1		0xF1
#define DS90_REG_FPD3_RX_ID2		0xF2
#define DS90_REG_FPD3_RX_ID3		0xF3
#define DS90_REG_FPD3_RX_ID4		0xF4
#define DS90_REG_FPD3_RX_ID5		0xF5
#define DS90_REG_I2C_RX0_ID		0xF8
#define DS90_REG_I2C_RX1_ID		0xF9

struct ds90_rxport {
	/* Errors and anomalies counters */
	u64 bcc_crc_error_count;
	u64 bcc_seq_error_count;
	u64 line_len_unstable_count;
	u64 line_len_chg_count;
	u64 fpd3_encode_error_count;
	u64 buffer_error_count;
	u64 line_cnt_chg_count;
	u64 csi_rx_sts_length_err_count;
	u64 csi_rx_sts_cksum_err_count;
	u64 csi_rx_sts_ecc2_err_count;
	u64 csi_rx_sts_ecc1_err_count;

	struct i2c_client *ser_client;
	unsigned short     ser_alias; /* ser i2c alias (lower 7 bits) */
	bool               locked;

	struct ds90_data  *ds90;  /* Owner */
	unsigned short     nport; /* Port number, and index in ds90->rxport[] */
};

struct ds90_gpio {
	bool                    output : 1; /* Direction */
	unsigned int            source : 3;
	unsigned int            func   : 3;
};

struct ds90_data {
	struct i2c_client      *client;
	struct i2c_mux_core    *muxc; /* i2c-mux with ATR */
	struct regmap          *regmap;
	struct gpio_desc       *reset_gpio;
	struct task_struct     *kthread;
	struct ds90_rxport     *rxport[DS90_NUM_RXPORTS];
	struct ds90_gpio       *gpio  [DS90_NUM_GPIOS];

	struct v4l2_subdev          sd;
	struct media_pad            pads[DS90_V4L2_NUM_PADS];
	struct v4l2_mbus_framefmt   fmt[DS90_V4L2_NUM_PADS];

	struct mutex            lock; /* Lock ATR table AND RX port selection */

	/* Address Translator alias-to-slave map table */
	size_t       atr_alias_num; /* Number of aliases configured */
	u16          atr_alias_id[DS90_MAX_POOL_ALIASES]; /* 0 = no alias */
	u16          atr_slave_id[DS90_MAX_POOL_ALIASES]; /* 0 = not in use */
};

#define sd_to_ds90(_sd) container_of(_sd, struct ds90_data, sd)

/* -----------------------------------------------------------------------------
 * Basic device access
 */

static bool ds90_is_volatile_reg(struct device *dev, unsigned int reg)
{
	return (reg == DS90_REG_INTERRUPT_STS                  ||
		reg == DS90_REG_RX_PORT_STS1                   ||
		reg == DS90_REG_RX_PORT_STS2                   ||
		reg == DS90_REG_CSI_RX_STS                     ||
		reg == DS90_REG_CSI_TX_ISR);
}

const struct regmap_config ds90_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.volatile_reg = ds90_is_volatile_reg,
};

static int ds90_read(const struct ds90_data *ds90,
		     unsigned int reg,
		     unsigned int *val)
{
	int err;

	err = regmap_read(ds90->regmap, reg, val);
	if (err)
		dev_err(&ds90->client->dev,
			"Cannot read register 0x%02x (%d)!\n", reg, err);

	return err;
}

static int ds90_write(const struct ds90_data *ds90,
		      unsigned int reg,
		      unsigned int val)
{
	int err;

	err = regmap_write(ds90->regmap, reg, val);
	if (err)
		dev_err(&ds90->client->dev,
			"Cannot write register 0x%02x (%d)!\n", reg, err);

	return err;
}

static void ds90_reset(const struct ds90_data *ds90, bool keep_reset)
{
	gpiod_set_value_cansleep(ds90->reset_gpio, 0);
	usleep_range(3000, 6000); /* min 2 ms */

	if (!keep_reset) {
		gpiod_set_value_cansleep(ds90->reset_gpio, 1);
		usleep_range(2000, 4000); /* min 1 ms */
	}
}

/* Select a port for register reading and writing */
static int ds90_rxport_select(struct ds90_data *ds90, unsigned nport)
{
	int err = ds90_write(ds90, DS90_REG_FPD3_PORT_SEL,
			     (nport << 4) | BIT(nport));

	return err;
}

/* -----------------------------------------------------------------------------
 * CSI port
 */

static void ds90_csi_handle_events(struct ds90_data *ds90)
{
	struct device *dev = &ds90->client->dev;
	unsigned int csi_tx_isr;
	int err;

	err = ds90_read(ds90, DS90_REG_CSI_TX_ISR, &csi_tx_isr);

	if (!err) {
		if (csi_tx_isr & DS90_REG_CSI_TX_ISR_IS_CSI_SYNC_ERROR)
			dev_warn(dev, "CSI_SYNC_ERROR\n");

		if (csi_tx_isr & DS90_REG_CSI_TX_ISR_IS_CSI_PASS_ERROR)
			dev_warn(dev, "CSI_PASS_ERROR\n");
	}
}

/* -----------------------------------------------------------------------------
 * I2C-MUX with ATR (address translator)
 */

static int ds90_mux_attach_client(struct i2c_mux_core *muxc,
				  u32 chan_id,
				  const struct i2c_board_info *info,
				  struct i2c_client *client,
				  u16 *alias_id)
{
	struct ds90_data **ds90p = i2c_mux_priv(muxc);
	struct ds90_data *ds90 = *ds90p;
	struct ds90_rxport *rxport = ds90->rxport[chan_id];
	struct device *dev = &ds90->client->dev;
	u16 alias = 0;
	int reg_idx;
	int pool_idx;
	int err = 0;

	mutex_lock(&ds90->lock);

	/* Find unused alias in table */

	for (pool_idx = 0; pool_idx < ds90->atr_alias_num; pool_idx++)
		if (ds90->atr_slave_id[pool_idx] == 0)
			break;

	if (pool_idx == ds90->atr_alias_num) {
		dev_warn(dev, "rx%d: alias pool exhausted\n", rxport->nport);
		err = -EADDRNOTAVAIL;
		goto out;
	}

	alias = ds90->atr_alias_id[pool_idx];

	/* Find first unused alias register */

	ds90_rxport_select(ds90, rxport->nport);

	for (reg_idx = 0; reg_idx < DS90_NUM_SLAVE_ALIASES; reg_idx++) {
		unsigned int regval;

		err = ds90_read(ds90, DS90_REG_SLAVE_ALIAS(reg_idx), &regval);
		if (!err && regval == 0)
			break;
	}

	if (reg_idx == DS90_NUM_SLAVE_ALIASES) {
		dev_warn(dev, "rx%d: all aliases in use\n", rxport->nport);
		err = -EADDRNOTAVAIL;
		goto out;
	}

	/* Map alias to slave */

	ds90_write(ds90, DS90_REG_SLAVE_ID(reg_idx), client->addr << 1);
	ds90_write(ds90, DS90_REG_SLAVE_ALIAS(reg_idx), alias << 1);

	ds90->atr_slave_id[pool_idx] = client->addr;

	*alias_id = alias; /* tell the mux which alias we chose */

	dev_info(dev, "rx%d: map alias 0x%02x to client 0x%02x\n",
		 rxport->nport, alias, client->addr);

out:
	mutex_unlock(&ds90->lock);
	return err;
}

static void ds90_mux_detach_client(struct i2c_mux_core *muxc,
				   u32 chan_id,
				   struct i2c_client *client)
{
	struct ds90_data **ds90p = i2c_mux_priv(muxc);
	struct ds90_data *ds90 = *ds90p;
	struct ds90_rxport *rxport = ds90->rxport[chan_id];
	struct device *dev = &ds90->client->dev;
	u16 alias = 0;
	int reg_idx;
	int pool_idx;

	mutex_lock(&ds90->lock);

	/* Find alias mapped to this client */

	for (pool_idx = 0; pool_idx < ds90->atr_alias_num; pool_idx++)
		if (ds90->atr_slave_id[pool_idx] == client->addr)
			break;

	if (pool_idx == ds90->atr_alias_num) {
		dev_err(dev, "rx%d: client 0x%02x is not mapped!\n",
			rxport->nport, client->addr);
		goto out;
	}

	alias = ds90->atr_alias_id[pool_idx];

	/* Find alias register used for this client */

	ds90_rxport_select(ds90, rxport->nport);

	for (reg_idx = 0; reg_idx < DS90_NUM_SLAVE_ALIASES; reg_idx++) {
		unsigned int regval;
		int err;

		err = ds90_read(ds90, DS90_REG_SLAVE_ALIAS(reg_idx), &regval);
		if (!err && regval == (alias << 1))
			break;
	}

	if (reg_idx == DS90_NUM_SLAVE_ALIASES) {
		dev_err(dev, "rx%d: cannot find alias 0x%02x reg (client 0x%02x)!\n",
			rxport->nport, alias, client->addr);
		goto out;
	}

	/* Unmap */

	ds90_write(ds90, DS90_REG_SLAVE_ALIAS(reg_idx), 0);
	ds90->atr_slave_id[pool_idx] = 0;

	dev_info(dev, "rx%d: unmapped alias 0x%02x from client 0x%02x\n",
		 rxport->nport, alias, client->addr);

out:
	mutex_unlock(&ds90->lock);
}

static const struct i2c_mux_attach_operations ds90_mux_attach_ops = {
	.i2c_mux_attach_client = ds90_mux_attach_client,
	.i2c_mux_detach_client = ds90_mux_detach_client,
};

static int ds90_mux_select(struct i2c_mux_core *muxc, u32 chan_id)
{
	struct ds90_data **ds90p = i2c_mux_priv(muxc);
	struct ds90_data *ds90 = *ds90p;

	return ds90_rxport_select(ds90, chan_id);
}

/* -----------------------------------------------------------------------------
 * RX ports
 */

static ssize_t locked_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf);
static ssize_t status_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf);

static struct device_attribute dev_attr_locked[] = {
	__ATTR_RO(locked),
	__ATTR_RO(locked),
};

static struct device_attribute dev_attr_status[] = {
	__ATTR_RO(status),
	__ATTR_RO(status),
};

static struct attribute *ds90_rxport0_attrs[] = {
	&dev_attr_locked[0].attr,
	&dev_attr_status[0].attr,
	NULL
};

static struct attribute *ds90_rxport1_attrs[] = {
	&dev_attr_locked[1].attr,
	&dev_attr_status[1].attr,
	NULL
};

static ssize_t locked_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int nport = (attr - dev_attr_locked);
	const struct ds90_data *ds90 = dev_get_drvdata(dev);
	const struct ds90_rxport *rxport = ds90->rxport[nport];

	return scnprintf(buf, PAGE_SIZE, "%d", rxport->locked);
}

static ssize_t status_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int nport = (attr - dev_attr_status);
	const struct ds90_data *ds90 = dev_get_drvdata(dev);
	const struct ds90_rxport *rxport = ds90->rxport[nport];

	return scnprintf(buf, PAGE_SIZE,
			 "bcc_crc_error_count = %llu\n"
			 "bcc_seq_error_count = %llu\n"
			 "line_len_unstable_count = %llu\n"
			 "line_len_chg_count = %llu\n"
			 "fpd3_encode_error_count = %llu\n"
			 "buffer_error_count = %llu\n"
			 "line_cnt_chg_count = %llu\n"
			 "csi_rx_sts_length_err_count = %llu\n"
			 "csi_rx_sts_cksum_err_count = %llu\n"
			 "csi_rx_sts_ecc2_err_count = %llu\n"
			 "csi_rx_sts_ecc1_err_count = %llu\n",
			 rxport->bcc_crc_error_count,
			 rxport->bcc_seq_error_count,
			 rxport->line_len_unstable_count,
			 rxport->line_len_chg_count,
			 rxport->fpd3_encode_error_count,
			 rxport->buffer_error_count,
			 rxport->line_cnt_chg_count,
			 rxport->csi_rx_sts_length_err_count,
			 rxport->csi_rx_sts_cksum_err_count,
			 rxport->csi_rx_sts_ecc2_err_count,
			 rxport->csi_rx_sts_ecc1_err_count);
}

struct attribute_group ds90_rxport_attr_group[] = {
	{ .name = "rx0", .attrs = ds90_rxport0_attrs },
	{ .name = "rx1", .attrs = ds90_rxport1_attrs },
};

/*
 * Instantiate serializer and i2c adapter for the just-locked remote
 * end.
 *
 * @note Must be called with ds90->lock not held! The added i2c
 * adapter will probe new slaves, which can request i2c transfers,
 * ending up in calling ds90_mux_attach_client() where the lock is
 * taken.
 */
static int ds90_rxport_add_serializer(struct ds90_data *ds90, int nport)
{
	struct ds90_rxport *rxport = ds90->rxport[nport];
	struct device *dev = &ds90->client->dev;
	struct i2c_board_info ser_info = { .type = "ds90ub953-q1" };
	struct i2c_client *client;
	int err;

	/*
	 * Adding the serializer under the rxport-specific adapter
	 * would be cleaner, but it would need tweaks to bypass the
	 * alias table. Adding to the upstream adapter is way simpler.
	 */
	ser_info.addr = rxport->ser_alias;
	client = i2c_new_device(ds90->client->adapter, &ser_info);
	if (!client) {
		dev_err(dev, "rx%d: cannot add %s i2c device",
			nport, ser_info.type);
		return -EIO;
	}
	rxport->ser_client = client;

	err = i2c_mux_add_adapter(ds90->muxc, 0, nport, 0);
	if (err) {
		dev_err(dev, "rx%d: cannot add adapter", nport);
		i2c_unregister_device(rxport->ser_client);
		rxport->ser_client = NULL;
	}

	return err;
}

static void ds90_rxport_remove_serializer(struct ds90_data *ds90, int nport)
{
	struct ds90_rxport *rxport = ds90->rxport[nport];

	i2c_mux_del_adapters(ds90->muxc); /* FIXME remove one only */

	if (rxport->ser_client) {
		i2c_unregister_device(rxport->ser_client);
		rxport->ser_client = NULL;
	}
}

/*
 * Map a local GPIO output to a back-channel GPIO number.
 *
 * Example: GPIO<N> from SoC -> GPIO<X> input at deser -> GPIO<Y> on FPD-3 link.
 */
static int ds90_rxport_map_bc_gpios(struct ds90_data *ds90,
				    int nport,
				    const struct device_node *np)
{
	struct device *dev = &ds90->client->dev;
	const __be32 *bc_gpio_map;
	int bc_gpio_map_len;
	struct device_node *local_gpio_np;
	const __be32 *local_gpio_regp;
	u32 bc_gpio_idx;
	u32 local_gpio_ph;
	u32 local_gpio_reg;
	unsigned int reg;
	unsigned reg_shift;
	int err;
	int i;

	bc_gpio_map = of_get_property(np, "bc-gpio-map", &bc_gpio_map_len);
	if (!bc_gpio_map) {
		dev_dbg(dev, "rx%d: bc-gpio-map NOT FOUND \n", nport);
		return -ENOENT;
	}

	bc_gpio_map_len /= sizeof(*bc_gpio_map);

	for (i = 0; i + 2 <= bc_gpio_map_len; i += 2) {
		bc_gpio_idx = be32_to_cpu(bc_gpio_map[i]);
		local_gpio_ph = be32_to_cpu(bc_gpio_map[i + 1]);

		if (bc_gpio_idx >= DS90_NUM_BC_GPIOS)
			continue;

		local_gpio_np = of_find_node_by_phandle(local_gpio_ph);
		if (!local_gpio_np)
			continue;

		local_gpio_regp = of_get_property(local_gpio_np, "reg", NULL);
		if (!local_gpio_regp)
			goto put_and_continue;

		local_gpio_reg = be32_to_cpup(local_gpio_regp);
		if (local_gpio_reg >= DS90_NUM_GPIOS)
			goto put_and_continue;

		reg = DS90_REG_BC_GPIO_CTL(bc_gpio_idx / 2);
		reg_shift = (bc_gpio_idx % 2) * 4;

		dev_dbg(dev, "rx%d: BC GPIO %d from local GPIO %d\n",
			nport, bc_gpio_idx, local_gpio_reg);

		err = regmap_update_bits(ds90->regmap, reg, 0xf << reg_shift,
					 local_gpio_reg << reg_shift);
		if (err)
			dev_err(dev, "rx%d: Cannot update reg 0x%02x (%d)\n",
				nport, reg, err);

	put_and_continue:
		of_node_put(local_gpio_np);
	}

	return 0;
}

static int ds90_rxport_probe_one(struct ds90_data *ds90,
				 const struct device_node *np)
{
	struct device *dev = &ds90->client->dev;
	struct ds90_rxport *rxport;
	u32 ser_alias;
	u32 nport;
	int err;

	if (of_property_read_u32(np, "reg", &nport) != 0 ||
	    nport >= DS90_NUM_RXPORTS)
		return -EINVAL;

	if (ds90->rxport[nport]) {
		dev_err(dev, "OF: %s: reg value %d is duplicated\n",
			of_node_full_name(np), nport);
		return -EADDRINUSE;
	}

	if (of_property_read_u32(np, "ser-i2c-alias", &ser_alias) != 0 ||
	    ser_alias == 0) {
		dev_err(dev, "OF: %s: invalid ser-i2c-alias\n",
			of_node_full_name(np));
		return -EINVAL;
	}

	rxport = devm_kzalloc(dev, sizeof(*rxport), GFP_KERNEL);
	if (!rxport)
		return -ENOMEM;

	rxport->nport = nport;
	rxport->ser_alias = ser_alias;
	rxport->ds90 = ds90;

	dev_dbg(dev, "OF: rxport[%d], alias=0x%02x\n", nport, ser_alias);

	ds90->rxport[nport] = rxport; /* Needed by successive calls */

	ds90_rxport_select(ds90, nport);

	ds90_rxport_map_bc_gpios(ds90, nport, np);

	regmap_update_bits(ds90->regmap, DS90_REG_INTERRUPT_CTL,
			   DS90_REG_INTERRUPT_CTL_IE_RX(nport), ~0);

	/* Enable all interrupt sources from this port */
	ds90_write(ds90, DS90_REG_PORT_ICR_HI, 0x07);
	ds90_write(ds90, DS90_REG_PORT_ICR_LO, 0x7f);

	regmap_update_bits(ds90->regmap,
			   DS90_REG_BCC_CONFIG,
			   DS90_REG_BCC_CONFIG_I2C_PASS_THROUGH, ~0);

	/* Enable I2C communication to the serializer via the alias addr */
	ds90_write(ds90, DS90_REG_SER_ALIAS_ID, rxport->ser_alias << 1);

	err = sysfs_create_group(&dev->kobj, &ds90_rxport_attr_group[nport]);
	if (err) {
		dev_err(dev, "rx%d: failed creating sysfs group", nport);
		goto err_sysfs;
	}

	return 0;

err_sysfs:
	ds90->rxport[nport] = NULL;
	return err;
}

static void ds90_rxport_remove_one(struct ds90_data *ds90, int nport)
{
	struct device *dev = &ds90->client->dev;

	ds90_rxport_remove_serializer(ds90, nport);
	sysfs_remove_group(&dev->kobj, &ds90_rxport_attr_group[nport]);
}

static int ds90_rxport_probe(struct ds90_data *ds90)
{
	struct device_node *ds90_node = ds90->client->dev.of_node;
	struct i2c_adapter *parent_adap = ds90->client->adapter;
	struct device *dev = &ds90->client->dev;
	const struct device_node *rxports_node;
	struct device_node *rxport_node;
	int err = 0;
	int i;

	/*
	 * TODO Maybe could be simplified by allocating the muxc in
	 *      ds90_probe() with the ds90 as its "priv" data?
	 *      Code: i2c_mux_alloc(parent_adap, DS90_NUM_RXPORTS, sizeof(ds90_data), ...)
	 *            ds90_data *ds90 = i2c_mux_priv(muxc)
	 */
	struct ds90_data **mux_data;
	ds90->muxc = i2c_mux_alloc(parent_adap, dev, DS90_NUM_RXPORTS,
				   sizeof(*mux_data),
				   I2C_MUX_LOCKED | I2C_MUX_ATR,
				   ds90_mux_select, NULL,
				   &ds90_mux_attach_ops);
	if (!ds90->muxc)
		return -ENOMEM;
	mux_data = i2c_mux_priv(ds90->muxc);
	*mux_data = ds90;

	rxports_node = of_get_child_by_name(ds90_node, "rxports");
	if (!rxports_node) {
		dev_warn(dev, "OF: no rxports defined!\n");
	} else {
		for_each_child_of_node(rxports_node, rxport_node) {
			err = ds90_rxport_probe_one(ds90, rxport_node);
			if (err)
				break;
		}
		of_node_put(rxport_node);
	}

	if (err)
		for (i = 0; i < DS90_NUM_RXPORTS; i++)
			if (ds90->rxport[i])
				ds90_rxport_remove_one(ds90, i);

	return err;
}

static void ds90_rxport_remove(struct ds90_data *ds90)
{
	int i;

	i2c_mux_del_adapters(ds90->muxc);

	for (i = 0; i < DS90_NUM_RXPORTS; i++)
		if (ds90->rxport[i])
			ds90_rxport_remove_one(ds90, i);
}

static void ds90_rxport_handle_events(struct ds90_data *ds90, int nport)
{
	struct ds90_rxport *rxport = ds90->rxport[nport];
	struct device *dev = &ds90->client->dev;
	unsigned int rx_port_sts1;
	unsigned int rx_port_sts2;
	unsigned int csi_rx_sts;
	bool locked;
	int err;

	mutex_lock(&ds90->lock);

	/* Select port for register reading and writing */
	err = ds90_rxport_select(ds90, nport);

	/* Read interrupts (also clears most of them) */
	if (!err)
		err = ds90_read(ds90, DS90_REG_RX_PORT_STS1, &rx_port_sts1);
	if (!err)
		err = ds90_read(ds90, DS90_REG_RX_PORT_STS2, &rx_port_sts2);
	if (!err)
		err = ds90_read(ds90, DS90_REG_CSI_RX_STS,   &csi_rx_sts);

	mutex_unlock(&ds90->lock);

	if (err)
		return;

	if (rx_port_sts1 & DS90_REG_RX_PORT_STS1_BCC_CRC_ERROR)
		rxport->bcc_crc_error_count++;

	if (rx_port_sts1 & DS90_REG_RX_PORT_STS1_BCC_SEQ_ERROR)
		rxport->bcc_seq_error_count++;

	if (rx_port_sts2 & DS90_REG_RX_PORT_STS2_LINE_LEN_UNSTABLE)
		rxport->line_len_unstable_count++;

	if (rx_port_sts2 & DS90_REG_RX_PORT_STS2_LINE_LEN_CHG)
		rxport->line_len_chg_count++;

	if (rx_port_sts2 & DS90_REG_RX_PORT_STS2_FPD3_ENCODE_ERROR)
		rxport->fpd3_encode_error_count++;

	if (rx_port_sts2 & DS90_REG_RX_PORT_STS2_BUFFER_ERROR)
		rxport->buffer_error_count++;

	if (rx_port_sts2 & DS90_REG_RX_PORT_STS2_LINE_CNT_CHG)
		rxport->line_cnt_chg_count++;

	if (csi_rx_sts & DS90_REG_CSI_RX_STS_LENGTH_ERR)
		rxport->csi_rx_sts_length_err_count++;

	if (csi_rx_sts & DS90_REG_CSI_RX_STS_CKSUM_ERR)
		rxport->csi_rx_sts_cksum_err_count++;

	if (csi_rx_sts & DS90_REG_CSI_RX_STS_ECC2_ERR)
		rxport->csi_rx_sts_ecc2_err_count++;

	if (csi_rx_sts & DS90_REG_CSI_RX_STS_ECC1_ERR)
		rxport->csi_rx_sts_ecc1_err_count++;

	/* Update locked status */
	locked = rx_port_sts1 & DS90_REG_RX_PORT_STS1_LOCK_STS;
	if (locked && !rxport->locked) {
		dev_info(dev, "rx%d LOCKED\n", nport);
		/* See note about locking in ds90_rxport_add_serializer()! */
		ds90_rxport_add_serializer(ds90, nport);
	}
	else if (!locked && rxport->locked) {
		dev_info(dev, "rx%d NOT LOCKED\n", nport);
		ds90_rxport_remove_serializer(ds90, nport);
	}
	rxport->locked = locked;
}

/* -----------------------------------------------------------------------------
 * GPIOs
 */

/* TODO struct ds90_gpio is not used outside of this function. Remove it??? */
static int ds90_gpio_probe_one(struct ds90_data *ds90, struct device_node *np)
{
	struct device *dev = &ds90->client->dev;
	struct ds90_gpio *gpio;
	u32 reg;
	u32 source;
	u32 func;

	if (of_property_read_u32(np, "reg", &reg) != 0 ||
	    reg >= DS90_NUM_GPIOS) {
		dev_err(dev, "OF: %s: invalid reg\n", of_node_full_name(np));
		return -EINVAL;
	}

	if (ds90->gpio[reg]) {
		dev_err(dev, "OF: %s: reg value %d is duplicated\n",
			of_node_full_name(np), reg);
		return -EADDRINUSE;
	}

	gpio = devm_kzalloc(dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->output = false; /* A safe default */

	if (of_property_read_bool(np, "output")) {
		if (of_property_read_u32(np, "source", &source) == 0 &&
		    of_property_read_u32(np, "function", &func) == 0 &&
		    source < DS90_GPIO_NSOURCES  &&
		    func   < DS90_GPIO_NFUNCS) {
			gpio->output = true;
			gpio->source = source;
			gpio->func   = func;
		} else {
			dev_err(dev,
				"OF: %s: incorrect output parameters, fallback to input!\n",
				of_node_full_name(np));
		}
	} else if (!of_property_read_bool(np, "input")) {
		dev_err(dev,
			"OF: %s: no direction specified, fallback to input!\n",
			of_node_full_name(np));
	}

	if (gpio->output)
		dev_dbg(dev, "OF: gpio[%d], output, source %d, function %d\n",
			reg, gpio->source, gpio->func);
	else
		dev_dbg(dev, "OF: gpio[%d], input\n", reg);

	ds90->gpio[reg] = gpio;

	if (gpio->output) {
		u8 pin_ctl = gpio->source << 5 | gpio->func << 2 | 1;

		regmap_update_bits(ds90->regmap, DS90_REG_GPIO_INPUT_CTL,
				   BIT(reg), 0);
		ds90_write(ds90, DS90_REG_GPIO_PIN_CTL(reg), pin_ctl);
	} else {
		regmap_update_bits(ds90->regmap, DS90_REG_GPIO_INPUT_CTL,
				   BIT(reg), ~0);
	}

	return 0;
}

static int ds90_gpio_probe(struct ds90_data *ds90)
{
	struct device_node *ds90_node = ds90->client->dev.of_node;
	struct device *dev = &ds90->client->dev;
	const struct device_node *gpios_node;
	struct device_node *gpio_node;
	int err = 0;

	gpios_node = of_get_child_by_name(ds90_node, "gpios");
	if (!gpios_node) {
		dev_warn(dev, "OF: no gpios defined!\n");
	} else {
		for_each_child_of_node(gpios_node, gpio_node) {
			err = ds90_gpio_probe_one(ds90, gpio_node);
			if (err)
				break;
		}
		of_node_put(gpio_node);
	}

	return err;
}

/* -----------------------------------------------------------------------------
 * V4L2
 */

static int ds90_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ds90_data *ds90 = sd_to_ds90(sd);
	struct device *dev = &ds90->client->dev;

	dev_info(dev, "%s: TODO\n", __func__);

	return 0;
}

static int ds90_enum_mbus_code(struct v4l2_subdev *sd,
			       struct v4l2_subdev_pad_config *cfg,
			       struct v4l2_subdev_mbus_code_enum *code)
{
	struct ds90_data *ds90 = sd_to_ds90(sd);
	struct device *dev = &ds90->client->dev;

	dev_info(dev, "%s: TODO\n", __func__);

	return 0;
}

static struct v4l2_mbus_framefmt *
ds90_get_pad_format(struct ds90_data *ds90,
		    struct v4l2_subdev_pad_config *cfg,
		    unsigned int pad, u32 which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(&ds90->sd, cfg, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &ds90->fmt[pad];
	default:
		return NULL;
	}
}

static int ds90_set_fmt(struct v4l2_subdev *sd,
			struct v4l2_subdev_pad_config *cfg,
			struct v4l2_subdev_format *format)
{
	struct ds90_data *ds90 = sd_to_ds90(sd);
	struct device *dev = &ds90->client->dev;

	dev_info(dev, "%s: TODO\n", __func__);

	return 0;
}

static int ds90_get_fmt(struct v4l2_subdev *sd,
			struct v4l2_subdev_pad_config *cfg,
			struct v4l2_subdev_format *format)
{
	struct ds90_data *ds90 = sd_to_ds90(sd);
	struct v4l2_mbus_framefmt *cfg_fmt;
	struct device *dev = &ds90->client->dev;

	dev_dbg(dev, "pad %d which %d", format->pad, format->which);

	if (format->pad >= DS90_V4L2_NUM_PADS)
		return -EINVAL;

	cfg_fmt = ds90_get_pad_format(ds90, cfg, format->pad, format->which);
	if (!cfg_fmt)
		return -EINVAL;

	format->format = *cfg_fmt;

	return 0;
}

static int ds90_get_frame_desc(struct v4l2_subdev *sd,
			       unsigned int pad,
			       struct v4l2_mbus_frame_desc *fd)
{
	struct ds90_data *ds90 = sd_to_ds90(sd);
	struct device *dev = &ds90->client->dev;

	dev_info(dev, "%s: TODO\n", __func__);

	return 0;
}

static const struct v4l2_subdev_video_ops ds90_video_ops = {
	.s_stream	= ds90_s_stream,
};

static const struct v4l2_subdev_pad_ops ds90_pad_ops = {
	.enum_mbus_code = ds90_enum_mbus_code,
	.get_fmt	= ds90_get_fmt,
	.set_fmt	= ds90_set_fmt,
	.get_frame_desc = ds90_get_frame_desc,
};

static const struct v4l2_subdev_ops ds90_subdev_ops = {
	.video		= &ds90_video_ops,
	.pad		= &ds90_pad_ops,
};

static void ds90_init_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width		= 1920;
	fmt->height		= 1080;
	fmt->code		= MEDIA_BUS_FMT_SRGGB8_1X8;
	fmt->colorspace		= V4L2_COLORSPACE_SRGB;
	fmt->field		= V4L2_FIELD_NONE;
}

static int ds90_open(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	struct v4l2_mbus_framefmt *format;
	unsigned int i;

	for (i = 0; i < DS90_V4L2_NUM_PADS; i++) {
		format = v4l2_subdev_get_try_format(subdev, fh->pad, i);
		ds90_init_format(format);
	}

	return 0;
}

static const struct v4l2_subdev_internal_ops ds90_subdev_internal_ops = {
	.open = ds90_open,
};

/* -----------------------------------------------------------------------------
 * Core
 */

static int ds90_run(void *arg)
{
	struct ds90_data *ds90 = arg;
	unsigned int int_sts;
	int err;
	int i;

	while (1) {
		if (kthread_should_stop())
			break;

		err = ds90_read(ds90, DS90_REG_INTERRUPT_STS, &int_sts);

		if (!err && int_sts) {
			if (int_sts & DS90_REG_INTERRUPT_STS_IS_CSI_TX0)
				ds90_csi_handle_events(ds90);

			for (i = 0; i < DS90_NUM_RXPORTS; i++)
				if (int_sts & DS90_REG_INTERRUPT_STS_IS_RX(i) &&
				    ds90->rxport[i])
					ds90_rxport_handle_events(ds90, i);
		}

		msleep(1000);
	}

	return 0;
}

static int ds90_parse_dt(struct ds90_data *ds90)
{
	struct device_node *np = ds90->client->dev.of_node;
	struct device *dev = &ds90->client->dev;
	int n;

	if (!np) {
		dev_err(dev, "OF: no device tree node!\n");
		return -ENOENT;
	}

	n = of_property_read_variable_u16_array(np, "i2c-alias-pool",
						ds90->atr_alias_id,
						2, DS90_MAX_POOL_ALIASES);
	if (n < 0)
		dev_warn(dev,
			 "OF: no i2c-alias-pool, can't access remote I2C slaves");

	ds90->atr_alias_num = n;

	dev_dbg(dev, "i2c-alias-pool has %zu aliases", ds90->atr_alias_num);

	return 0;
}

static int ds90_probe(struct i2c_client *client,
		      const struct i2c_device_id *id)
{
	struct ds90_data *ds90;
	unsigned int rev_mask;
	int err;
	int i;

	ds90 = devm_kzalloc(&client->dev, sizeof(*ds90), GFP_KERNEL);
	if (!ds90)
		return -ENOMEM;

	ds90->client = client;

	err = ds90_parse_dt(ds90);
	if (err)
		goto err_parse_dt;

	/* get reset pin from DT */
	ds90->reset_gpio = devm_gpiod_get(&client->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ds90->reset_gpio)) {
		if (PTR_ERR(ds90->reset_gpio) != -EPROBE_DEFER)
			dev_err(&client->dev, "Cannot get reset GPIO");
		return PTR_ERR(ds90->reset_gpio);
	}

	mutex_init(&ds90->lock);

	i2c_set_clientdata(client, ds90);

	/* initialize regmap */
	ds90->regmap = devm_regmap_init_i2c(client, &ds90_regmap_config);
	if (IS_ERR(ds90->regmap)) {
		err = PTR_ERR(ds90->regmap);
		dev_err(&client->dev, "regmap init failed (%d)\n", err);
		goto err_regmap;
	}

	/* Runtime check register accessibility */
	ds90_reset(ds90, false);

	err = ds90_read(ds90, DS90_REG_REV_MASK, &rev_mask);
	if (err) {
		dev_err(&client->dev,
			"Cannot read first register (%d), abort\n", err);
		goto err_reg_read;
	}

	/* Init rxports, remote GPIOs and I2C */

	err = ds90_gpio_probe(ds90);
	if (err) {
		dev_err(&client->dev, "Error probing gpios (%d)\n", err);
		goto err_configure_deser_gpios;
	}

	err = ds90_rxport_probe(ds90);
	if (err)
		goto err_rxport_probe;

	/* V4L2 */

	for (i = 0; i < DS90_V4L2_NUM_PADS; i++)
		ds90_init_format(&ds90->fmt[i]);

	v4l2_i2c_subdev_init(&ds90->sd, client, &ds90_subdev_ops);

	/* Let both the I2C client and the subdev point to us */
	i2c_set_clientdata(client, ds90); /* v4l2_i2c_subdev_init writes it */
	v4l2_set_subdevdata(&ds90->sd, ds90);

	ds90->sd.internal_ops = &ds90_subdev_internal_ops;
	ds90->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	/* TODO MEDIA_ENT_F_VID_IF_BRIDGE (since kernel 4.13) is better? */
	ds90->sd.entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;

	ds90->pads[0].flags = MEDIA_PAD_FL_SINK;
	ds90->pads[1].flags = MEDIA_PAD_FL_SOURCE;
	err = media_entity_pads_init(&ds90->sd.entity,
				     DS90_V4L2_NUM_PADS, ds90->pads);
	if (err)
		goto err_pads_init;

	err = v4l2_async_register_subdev(&ds90->sd);
	if (err) {
		dev_err(&client->dev, "v4l2_async_register_subdev error %d\n", err);
		goto err_register_subdev;
	}

	/* Kick off */

	regmap_update_bits(ds90->regmap,
			   DS90_REG_INTERRUPT_CTL,
			   DS90_REG_INTERRUPT_CTL_INT_EN, ~0);

	ds90->kthread = kthread_run(ds90_run, ds90, dev_name(&client->dev));
	if (IS_ERR(ds90->kthread)) {
		err = PTR_ERR(ds90->kthread);
		dev_err(&client->dev, "Cannot create kthread (%d)\n", err);
		goto err_kthread;
	}

	dev_info(&client->dev, "Successfully probed (rev/mask %02x)\n", rev_mask);

	return 0;

err_kthread:
	v4l2_async_unregister_subdev(&ds90->sd);
err_register_subdev:
	media_entity_cleanup(&ds90->sd.entity);
err_pads_init:
	ds90_rxport_remove(ds90);
err_rxport_probe:
err_configure_deser_gpios:
err_reg_read:
	ds90_reset(ds90, true);
err_regmap:
	mutex_destroy(&ds90->lock);
err_parse_dt:
	return err;
}

static int ds90_remove(struct i2c_client *client)
{
	struct ds90_data *ds90 = i2c_get_clientdata(client);

	dev_info(&client->dev, "Removing\n");

	kthread_stop(ds90->kthread);
	v4l2_async_unregister_subdev(&ds90->sd);
	media_entity_cleanup(&ds90->sd.entity);
	ds90_rxport_remove(ds90);
	ds90_reset(ds90, true);
	mutex_destroy(&ds90->lock);

	return 0;
}

static const struct i2c_device_id ds90_id[] = {
	{ "ds90ub954-q1", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ds90_id);

#ifdef CONFIG_OF
static const struct of_device_id ds90_dt_ids[] = {
	{ .compatible = "ti,ds90ub954-q1", },
	{ }
};
MODULE_DEVICE_TABLE(of, ds90_dt_ids);
#endif

static struct i2c_driver ds90ub954_driver = {
	.probe		= ds90_probe,
	.remove		= ds90_remove,
	.id_table	= ds90_id,
	.driver = {
		.name	= "ds90ub954",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ds90_dt_ids),
	},
};

module_i2c_driver(ds90ub954_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Texas Instruments DS90UB954-Q1 CSI-2 dual deserializer driver");
MODULE_AUTHOR("Luca Ceresoli <luca@lucaceresoli.net>");
