#include "tw5864.h"

#define TW5864_IIC_RETRIES 30000

static int tw5864_smbus_xfer(struct i2c_adapter *adap, u16 addr,
			     unsigned short flags, char read_write, u8 command,
			     int size, union i2c_smbus_data *data);
static u32 tw5864_i2c_functionality(struct i2c_adapter *adap);

static const struct i2c_algorithm tw5864_i2c_algo = {
	.smbus_xfer    = tw5864_smbus_xfer,
	.functionality = tw5864_i2c_functionality,
};

static int tw5864_smbus_xfer(struct i2c_adapter *adap, u16 addr,
			     unsigned short flags, char read_write, u8 command,
			     int size, union i2c_smbus_data *data)
{
	struct tw5864_i2c_adap *ctx = adap->algo_data;
	struct tw5864_dev *dev = ctx->dev;
	int devid = ctx->devid;
	int retries = adap->retries;
	u32 first_write = BIT(24) | devid << 17 | addr << 8;
	u32 val;

	if (read_write == I2C_SMBUS_READ)
		first_write |= BIT(16);
	else
		first_write |= data->byte;

	if (size != I2C_SMBUS_BYTE_DATA)
		return -EIO;

	mutex_lock(&dev->i2c_lock);
	tw_writel(TW5864_IIC, first_write);
	do {
		val = tw_readl(TW5864_IIC);
	} while (!(val & BIT(24)) && --retries);
	mutex_unlock(&dev->i2c_lock);
	data->byte = val;

	if (!retries) {
		dev_err(&dev->pci->dev,
			"tw5864 i2c: out of %s attempts on devid 0x%x, addr 0x%x\n",
			read_write == I2C_SMBUS_READ ? "read" : "write",
			devid, addr);
		return -ETIMEDOUT;
	}

	return 0;
}

int tw5864_i2c_read(struct tw5864_dev *dev, u8 i2c_index, u8 offset, u8 *data)
{
	struct i2c_client *client = &dev->i2c[i2c_index].client;
	s32 ret;

	WARN_ON(i2c_index > 3);
	ret = i2c_smbus_read_byte_data(client, offset);
	*data = ret;
	return ret;
}

int tw5864_i2c_write(struct tw5864_dev *dev, u8 i2c_index, u8 offset, u8 data)
{
	struct i2c_client *client = &dev->i2c[i2c_index].client;

	WARN_ON(i2c_index > 3);
	return i2c_smbus_write_byte_data(client, offset, data);
}

static u32 tw5864_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_BYTE_DATA;
}

void tw5864_i2c_fini(struct tw5864_dev *dev)
{
	struct i2c_adapter *adap;
	int i;

	for (i = 0; i < 4; i++) {
		adap = &dev->i2c[i].adap;
		if (adap->algo_data) {
			i2c_del_adapter(adap);
			adap->algo_data = NULL;
		}
	}

	mutex_destroy(&dev->i2c_lock);
}

int tw5864_i2c_init(struct tw5864_dev *dev)
{
	int ret;
	int i;
	struct tw5864_i2c_adap *ctx;
	struct i2c_adapter *adap;
	struct i2c_client *client;

	tw_writel(TW5864_IIC_ENB, 1);
	tw_writel(TW5864_I2C_PHASE_CFG, 1);

	mutex_init(&dev->i2c_lock);

	dev->i2c[0].devid = 0x28; /* tw2865 */
	dev->i2c[1].devid = 0x29; /* tw2864 */
	dev->i2c[2].devid = 0x2a; /* tw2864 */
	dev->i2c[3].devid = 0x2b; /* tw2864 */

	for (i = 0; i < 4; i++) {
		ctx = &dev->i2c[i];
		adap = &ctx->adap;
		client = &ctx->client;

		ctx->dev = dev;
		snprintf(adap->name, sizeof(adap->name),
			 "tw5864 0x%02x", ctx->devid);
		adap->algo = &tw5864_i2c_algo;
		adap->algo_data = ctx;
		adap->timeout = msecs_to_jiffies(1000);
		adap->retries = TW5864_IIC_RETRIES;
		adap->dev.parent = &dev->pci->dev;

		ret = i2c_add_adapter(adap);
		if (ret) {
			adap->algo_data = NULL;
			break;
		}

		client->adapter = adap;
		client->addr = ctx->devid;
	}

	if (ret)
		tw5864_i2c_fini(dev);

	return ret;
}
