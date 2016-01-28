/*
 * F81532/F81534 USB to Serial Ports Bridge
 *
 * F81532 => 2 Serial Ports
 * F81534 => 4 Serial Ports
 *
 * Copyright (C) 2016 Tom Tsai (Tom_Tsai@fintek.com.tw)
 *
 * The F81532/F81534 had 1 control endpoint for setting, 1 endpoint bulk-out
 * for all serial port TX and 1 endpoint bulk-in for all serial port read in
 * (Read Data/MSR/LSR).
 *
 * Write URB is fixed with 512bytes, per serial port used 128Bytes.
 * It can be described by f81534_prepare_write_buffer()
 *
 * Read URB is 512Bytes max, per serial port used 128Bytes.
 * It can be described by f81534_process_read_urb() and maybe received with
 * 128x1,2,3,4 bytes.
 *
 * We can control M0(SD)/M1/M2 per ports to control outer transceiver. This IC
 * contains a internal flash to save configuration (UART Mode & GPIO). We can
 * use user-space tool to change the initial port mode data in flash.
 *
 * Features:
 * 1. F81532 is 1-to-2 & F81534 is 1-to-4 serial ports IC
 * 2. Support baudrate from B50 to B1500000 (excluding B1000000).
 * 3. The RTS signal can do auto-direction control by user-space tool.
 * 4. The 4x3 output-only open-drain pins for F81532/534 is designed for
 *    control outer devices (with our EVB for examples, the 4 sets of pins
 *    are designed to control transceiver mode). It's also controlled by
 *    user-space tool.
 * 5. User-space tool will save the configuration in internal storage and
 *    the IC will read it when power on or driver loaded.
 *
 *    Please reference https://bitbucket.org/hpeter/fintek-general/src/
 *    with f81534/tools to get user-space tool to change F81532/534
 *    setting. Please use it carefully.
 */
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/usb.h>
#include <linux/usb/serial.h>
#include <linux/serial_reg.h>
#include <linux/module.h>

/* Serial Port register Address */
#define SERIAL_BASE_ADDRESS		0x1200
#define DIVISOR_LATCH_LSB		(0x00 + SERIAL_BASE_ADDRESS)
#define DIVISOR_LATCH_MSB		(0x01 + SERIAL_BASE_ADDRESS)
#define INTERRUPT_ENABLE_REGISTER	(0x01 + SERIAL_BASE_ADDRESS)
#define FIFO_CONTROL_REGISTER		(0x02 + SERIAL_BASE_ADDRESS)
#define LINE_CONTROL_REGISTER		(0x03 + SERIAL_BASE_ADDRESS)
#define MODEM_CONTROL_REGISTER		(0x04 + SERIAL_BASE_ADDRESS)
#define MODEM_STATUS_REGISTER		(0x06 + SERIAL_BASE_ADDRESS)
#define CLK_SEL_REGISTER		(0x08 + SERIAL_BASE_ADDRESS)
#define CONFIG1_REGISTER		(0x09 + SERIAL_BASE_ADDRESS)
#define SADDRESS_REGISTER		(0x0a + SERIAL_BASE_ADDRESS)
#define SADEN_REGISTER			(0x0b + SERIAL_BASE_ADDRESS)

#define F81534_DEF_CONF_ADDRESS_START	0x3000
#define F81534_DEF_CONF_SIZE		8

#define F81534_CUSTOM_ADDRESS_START	0x2f00
#define F81534_CUSTOM_DATA_SIZE		0x10
#define F81534_CUSTOM_MAX_IDX		1
#define F81534_CUSTOM_NO_CUSTOM_DATA	(-1)
#define F81534_CUSTOM_VALID_TOKEN	0xf0
#define F81534_CONF_OFFSET		1
#define F81534_CONF_SIZE		4

#define F81534_MAX_DATA_BLOCK		64
#define F81534_MAX_BUS_RETRY		2000

/* default URB timeout for USB operations */
#define F81534_USB_MAX_RETRY		10
#define F81534_USB_TIMEOUT		1000
#define F81534_SET_GET_REGISTER		0xA0

#define F81534_NUM_PORT			4
#define F81534_UNUSED_PORT		0xff
#define F81534_WRITE_BUFFER_SIZE	512

#define IC_NAME				"f81534"
#define DRIVER_DESC			"Fintek F81532/F81534"
#define FINTEK_VENDOR_ID_1		0x1934
#define FINTEK_VENDOR_ID_2		0x2C42
#define FINTEK_DEVICE_ID		0x1202
#define F81534_MAX_TX_SIZE		100
#define F81534_RECEIVE_BLOCK_SIZE	128

#define F81534_TOKEN_RECEIVE		0x01
#define F81534_TOKEN_WRITE		0x02
#define F81534_TOKEN_TX_EMPTY		0x03
#define F81534_TOKEN_MSR_CHANGE		0x04

#define F81534_BUS_BUSY			0x03
#define F81534_BUS_IDLE			0x04
#define F81534_BUS_READ_DATA		0x1004
#define F81534_BUS_REG_STATUS		0x1003
#define F81534_BUS_REG_START		0x1002
#define F81534_BUS_REG_END		0x1001

#define F81534_CMD_READ			0x03
#define F81534_CMD_ENABLE_WR		0x06
#define F81534_CMD_PROGRAM		0x02
#define F81534_CMD_ERASE		0x20
#define F81534_CMD_READ_STATUS		0x05

#define F81534_MEDIA_BUSY_STATUS	0x03

#define F81534_1X_RXTRIGGER		0xc3
#define F81534_8X_RXTRIGGER		0xcf

#define F81534_DEFAULT_BAUD_RATE	9600
#define F81534_MAX_BAUDRATE		1500000

#define F81534_DELAY_READ_MSR		10

#define F81534_RS232_FLAG		0x00
#define F81534_RS485_FLAG		0x03
#define F81534_RS485_1_FLAG		0x01
#define F81534_MODE_MASK		0x03
#define F81534_PORT_CONF_RS485		BIT(0)
#define F81534_PORT_CONF_RS485_INVERT	BIT(1)
#define F81534_PORT_CONF_DISABLE_PORT	BIT(3)
#define F81534_PORT_CONF_NOT_EXIST_PORT	BIT(7)
#define F81534_PORT_UNAVAILABLE		\
	(F81534_PORT_CONF_DISABLE_PORT | F81534_PORT_CONF_NOT_EXIST_PORT)

#define F81534_RS485_MODE	BIT(4)
#define F81534_RS485_INVERT	BIT(5)

#define F81534_PIN_SET_DEFAULT	0x01
#define F81534_PIN_SET_MAX	0x07
#define F81534_PIN_SET_MIN	0x00

/*
 * For older configuration use. We'll transform it to newer setting after
 * load it. Please reference the comment of f81534_calc_num_ports()
 */
#define F81534_OLD_CONFIG_37	0x37
#define F81534_OLD_CONFIG_38	0x38
#define F81534_OLD_CONFIG_39	0x39

/*
 * Clock rate selector, always or-ed with CLKSEL_ENABLE_UART to enable
 * UART functional.
 */
#define CLKSEL_ENABLE_UART	BIT(0)
#define CLKSEL_1DOT846_MHZ	CLKSEL_ENABLE_UART
#define CLKSEL_18DOT46_MHZ	(BIT(1) | CLKSEL_ENABLE_UART)
#define CLKSEL_24_MHZ		(BIT(2) | CLKSEL_ENABLE_UART)
#define CLKSEL_14DOT77_MHZ	(BIT(1) | BIT(2) | CLKSEL_ENABLE_UART)

enum f81534_uart_mode {
	uart_mode_rs232,
	uart_mode_rs485,
	uart_mode_rs485_1,
	uart_mode_invalid,
};

/* Save for a control register and bit offset */
struct reg_value {
	const u16 reg_address;
	const u16 reg_offset;
};

/*
 * The following register is for F81532/534 output pin register maps to control
 * F81532/534 M0_SD/M1/M2 per port and we can reference f81438/439 transceiver
 * spec to get mode list. If you are not use F81438/439, please review
 * f81534_switch_gpio_mode() for desire gpio out value.
 *
 * For examples, we want to control F81532/534 port 0 M0_SD/M1/M2 to 0/0/1.
 * We'll do with following instructions.
 *
 *	1. set reg 0x2ae8 bit7 to 0 (M0_SD)
 *	2. set reg 0x2a90 bit5 to 0 (M1)
 *	3. set reg 0x2a90 bit4 to 1 (M2)
 *
 * F81438 Spec:
 * http://www.alldatasheet.com/datasheet-pdf/pdf/459082/FINTEK/F81438.html
 */
static const struct reg_value f81534_pin_control[4][3] = {
	/* M0_SD	M1		M2 */
	{{0x2ae8, 7}, {0x2a90, 5}, {0x2a90, 4}, },	/* port 0 pins */
	{{0x2ae8, 6}, {0x2ae8, 0}, {0x2ae8, 3}, },	/* port 1 pins */
	{{0x2a90, 0}, {0x2ae8, 2}, {0x2a80, 6}, },	/* port 2 pins */
	{{0x2a90, 3}, {0x2a90, 2}, {0x2a90, 1}, },	/* port 3 pins */
};

static const struct usb_device_id f81534_id_table[] = {
	{USB_DEVICE(FINTEK_VENDOR_ID_1, FINTEK_DEVICE_ID)},
	{USB_DEVICE(FINTEK_VENDOR_ID_2, FINTEK_DEVICE_ID)},
	{}			/* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, f81534_id_table);

struct f81534_serial_private {
	bool is_phy_port_not_empty[F81534_NUM_PORT];
	spinlock_t tx_empty_lock;
	u8 default_conf_data[F81534_DEF_CONF_SIZE];
	u32 setting_idx;
	atomic_t port_active[F81534_NUM_PORT];
};

struct f81534_port_private {
	u8 phy;
	u8 shadow_mcr;
	u8 shadow_lcr;
	u32 current_baud_rate;
	u32 current_baud_base;
	spinlock_t msr_lock;
	struct mutex msr_mutex;
	u8 shadow_msr;
	enum f81534_uart_mode uart_mode;
	u8 gpio_mode;
};

/*
 * Get the current logical port index of this device. e.g., If this port is
 * ttyUSB2 and start port is ttyUSB0, this will return 2.
 */
static int f81534_port_index(struct usb_serial_port *port)
{
	return port->port_number;
}

/*
 * Find logic serial port index with H/W phy index mapping. Due to our device
 * can be enable/disable port by internal storage to make the port phy no
 * continuously, we can use this to find phy & logical port mapping.
 */
static int f81534_phy_to_logic_port(struct usb_serial *serial, int phy)
{
	size_t count = 0, i;
	struct f81534_serial_private *serial_priv =
			usb_get_serial_data(serial);

	for (i = 0; i < phy; ++i) {
		if (serial_priv->default_conf_data[i] &
				F81534_PORT_UNAVAILABLE)
			continue;

		++count;
	}

	dev_dbg(&serial->dev->dev, "%s: phy: %d count: %zu\n", __func__, phy,
			count);
	return count;
}

static int f81534_set_normal_register(struct usb_device *dev, u16 reg, u8 data)
{
	size_t count = F81534_USB_MAX_RETRY;
	int status = 0;
	u8 *tmp;

	tmp = kmalloc(sizeof(u8), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	*tmp = data;

	/*
	 * Our device maybe not reply when heavily loading, We'll retry for
	 * F81534_USB_MAX_RETRY times.
	 */
	while (count--) {
		status = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
					 F81534_SET_GET_REGISTER,
					 USB_TYPE_VENDOR | USB_DIR_OUT,
					 reg, 0, tmp, sizeof(u8),
					 F81534_USB_TIMEOUT);
		if (status > 0)
			break;

		if (status == 0)
			status = -EIO;
	}

	if (status < 0) {
		dev_err(&dev->dev, "%s: reg: %x data: %x failed: %d\n",
				__func__, reg, data, status);
		kfree(tmp);
		return status;
	}

	kfree(tmp);
	return 0;
}

static int f81534_get_normal_register(struct usb_device *dev, u16 reg,
					u8 *data)
{
	size_t count = F81534_USB_MAX_RETRY;
	int status;
	u8 *tmp;

	tmp = kmalloc(sizeof(u8), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	/*
	 * Our device maybe not reply when heavily loading, We'll retry for
	 * F81534_USB_MAX_RETRY times.
	 */
	while (count--) {
		status = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
					 F81534_SET_GET_REGISTER,
					 USB_TYPE_VENDOR | USB_DIR_IN,
					 reg, 0, tmp, sizeof(u8),
					 F81534_USB_TIMEOUT);
		if (status > 0)
			break;

		if (status == 0)
			status = -EIO;
	}

	if (status < 0) {
		dev_err(&dev->dev, "%s: reg: %x failed: %d\n", __func__, reg,
				status);
		kfree(tmp);
		return status;
	}

	*data = *tmp;
	kfree(tmp);
	return 0;
}

static int f81534_set_mask_normal_register(struct usb_device *dev, u16 reg,
						u8 mask, u8 data)
{
	int status;
	u8 tmp;

	status = f81534_get_normal_register(dev, reg, &tmp);
	if (status)
		return status;

	tmp = (tmp & ~mask) | (mask & data);

	status = f81534_set_normal_register(dev, reg, tmp);
	if (status)
		return status;

	return 0;
}

static int f81534_setregister(struct usb_device *dev, u8 uart, u16 reg,
				u8 data)
{
	return f81534_set_normal_register(dev, reg + uart * 0x10, data);
}

static int f81534_getregister(struct usb_device *dev, u8 uart, u16 reg,
				u8 *data)
{
	return f81534_get_normal_register(dev, reg + uart * 0x10, data);
}

static int f81534_command_delay(struct usb_serial *usbserial)
{
	size_t count = F81534_MAX_BUS_RETRY;
	unsigned char tmp;
	int status;
	struct usb_device *dev = usbserial->dev;

	do {
		status = f81534_get_normal_register(dev, F81534_BUS_REG_STATUS,
							&tmp);
		if (status)
			return status;

		if (tmp & F81534_BUS_BUSY)
			continue;

		if (tmp & F81534_BUS_IDLE)
			break;

	} while (--count);

	if (!count)
		return -EIO;

	status = f81534_set_normal_register(dev, F81534_BUS_REG_STATUS,
				tmp & ~F81534_BUS_IDLE);
	if (status)
		return status;

	return 0;
}

static int f81534_get_normal_register_with_delay(struct usb_serial *usbserial,
							u16 reg, u8 *data)
{
	int status;
	struct usb_device *dev = usbserial->dev;

	status = f81534_get_normal_register(dev, reg, data);
	if (status)
		return status;

	status = f81534_command_delay(usbserial);
	if (status)
		return status;

	return 0;
}

static int f81534_set_normal_register_with_delay(struct usb_serial *usbserial,
							u16 reg, u8 data)
{
	int status;
	struct usb_device *dev = usbserial->dev;

	status = f81534_set_normal_register(dev, reg, data);
	if (status)
		return status;

	status = f81534_command_delay(usbserial);
	if (status)
		return status;

	return 0;
}

static int f81534_read_data(struct usb_serial *usbserial, u32 address,
				size_t size, unsigned char *buf)
{
	size_t read_size, count, block = 0;
	u16 reg_tmp;
	u8 tmp_buf[F81534_MAX_DATA_BLOCK];
	int status, offset;

	status = f81534_set_normal_register_with_delay(usbserial,
				F81534_BUS_REG_START, F81534_CMD_READ);
	if (status)
		return status;

	status = f81534_set_normal_register_with_delay(usbserial,
				F81534_BUS_REG_START, (address >> 16) & 0xff);
	if (status)
		return status;

	status = f81534_set_normal_register_with_delay(usbserial,
				F81534_BUS_REG_START, (address >> 8) & 0xff);
	if (status)
		return status;

	status = f81534_set_normal_register_with_delay(usbserial,
				F81534_BUS_REG_START, (address >> 0) & 0xff);
	if (status)
		return status;

	/* continuous read mode */
	do {
		read_size = min_t(u32, F81534_MAX_DATA_BLOCK, size);

		for (count = 0; count < read_size; ++count) {
			/* To write F81534_BUS_REG_END when final byte */
			if (size <= F81534_MAX_DATA_BLOCK && read_size ==
					count + 1)
				reg_tmp = F81534_BUS_REG_END;
			else
				reg_tmp = F81534_BUS_REG_START;

			/*
			 * dummy code, force IC to generate a read pulse, the
			 * set of value 0xf1 is dont care (any value is ok)
			 */
			status = f81534_set_normal_register_with_delay(
					usbserial, reg_tmp, 0xf1);
			if (status)
				return status;

			status = f81534_get_normal_register_with_delay(
						usbserial,
						F81534_BUS_READ_DATA,
						&tmp_buf[count]);
			if (status)
				return status;

			offset = count + block * F81534_MAX_DATA_BLOCK;
			buf[offset] = tmp_buf[count];
		}

		size -= read_size;
		++block;
	} while (size);

	return 0;
}

/*
 * This function maybe cause IC no workable, Please take this carefully.
 *
 * The function is used to modify the configuration area of this device
 * (F81534_CUSTOM_ADDRESS_START), please reference the comments of
 * f81534_calc_num_ports(). If wrong operation with this function, it'll make
 * the device malfunctional.
 */
static int f81534_write_data(struct usb_serial *usbserial, u32 address,
			     size_t size, unsigned char *buf)
{
	size_t write_size, count, block = 0;
	u16 reg_tmp;
	int offset, status;

	status = f81534_set_normal_register_with_delay(usbserial,
				F81534_BUS_REG_END, F81534_CMD_ENABLE_WR);
	if (status)
		return status;

	status = f81534_set_normal_register_with_delay(usbserial,
				F81534_BUS_REG_START, F81534_CMD_PROGRAM);
	if (status)
		return status;

	status = f81534_set_normal_register_with_delay(usbserial,
				F81534_BUS_REG_START, (address >> 16) & 0xff);
	if (status)
		return status;

	status = f81534_set_normal_register_with_delay(usbserial,
				F81534_BUS_REG_START, (address >> 8) & 0xff);
	if (status)
		return status;

	status = f81534_set_normal_register_with_delay(usbserial,
				F81534_BUS_REG_START, (address >> 0) & 0xff);
	if (status)
		return status;

	do {
		write_size = min_t(u32, F81534_MAX_DATA_BLOCK, size);

		for (count = 0; count < write_size; ++count) {
			offset = count + block * F81534_MAX_DATA_BLOCK;

			if (size <= F81534_MAX_DATA_BLOCK && write_size ==
					count + 1)
				reg_tmp = F81534_BUS_REG_END;
			else
				reg_tmp = F81534_BUS_REG_START;

			status = f81534_set_normal_register_with_delay(
						usbserial, reg_tmp,
						buf[offset]);
			if (status)
				return status;
		}

		size -= write_size;
		++block;
	} while (size);

	return 0;
}

/*
 * This function maybe cause IC no workable, Please take this carefully.
 *
 * The function is used to clear the configuration area of this device
 * (F81534_CUSTOM_ADDRESS_START), the internal storage is flash, so we should
 * erase the sector before write data. If wrong operation with this function,
 * it'll make the device malfunctional.
 */
static int f81534_erase_sector(struct usb_serial *usbserial, int address)
{
	u8 current_status = 0;
	int status;
	size_t count = F81534_MAX_BUS_RETRY;

	status = f81534_set_normal_register_with_delay(usbserial,
				F81534_BUS_REG_END, F81534_CMD_ENABLE_WR);
	if (status)
		return status;

	status = f81534_set_normal_register_with_delay(usbserial,
			F81534_BUS_REG_START, F81534_CMD_ERASE);
	if (status)
		return status;

	status = f81534_set_normal_register_with_delay(usbserial,
			F81534_BUS_REG_START, (address >> 16) & 0xff);
	if (status)
		return status;

	status = f81534_set_normal_register_with_delay(usbserial,
			F81534_BUS_REG_START, (address >> 8) & 0xff);
	if (status)
		return status;

	status = f81534_set_normal_register_with_delay(usbserial,
			F81534_BUS_REG_END, (address >> 0) & 0xff);
	if (status)
		return status;

	while (--count) {
		status = f81534_set_normal_register_with_delay(usbserial,
						F81534_BUS_REG_START,
						F81534_CMD_READ_STATUS);
		if (status)
			return status;

		/* dummy write, any value is acceptable */
		status = f81534_set_normal_register_with_delay(usbserial,
				F81534_BUS_REG_END, 0xff);
		if (status)
			return status;

		status = f81534_get_normal_register_with_delay(usbserial,
					F81534_BUS_READ_DATA, &current_status);
		if (status)
			return status;

		if (!(F81534_MEDIA_BUSY_STATUS & current_status)) {
			dev_dbg(&usbserial->dev->dev,
					"%s: data:%x, count:%zu, ok\n",
					__func__, current_status, count);
			break;
		}
	}

	return 0;
}

static int f81534_prepare_write_buffer(struct usb_serial_port *port,
					void *dest, size_t size)
{
	unsigned char *ptr = (unsigned char *)dest;
	struct f81534_port_private *port_priv = usb_get_serial_port_data(port);
	int port_num = port_priv->phy;

	/*
	 * The block layout is fixed with 4x128 Bytes, per 128 Bytes a port.
	 * index 0: port phy idx (e.g., 0,1,2,3)
	 * index 1: only F81534_TOKEN_WRITE
	 * index 2: serial out size
	 * index 3: fix to 0
	 * index 4~127: serial out data block
	 */
	ptr[F81534_RECEIVE_BLOCK_SIZE * 0] = 0;
	ptr[F81534_RECEIVE_BLOCK_SIZE * 1] = 1;
	ptr[F81534_RECEIVE_BLOCK_SIZE * 2] = 2;
	ptr[F81534_RECEIVE_BLOCK_SIZE * 3] = 3;
	ptr[F81534_RECEIVE_BLOCK_SIZE * port_num + 1] = F81534_TOKEN_WRITE;
	ptr[F81534_RECEIVE_BLOCK_SIZE * port_num + 3] = 0;
	ptr[F81534_RECEIVE_BLOCK_SIZE * port_num + 2] =
		kfifo_out_locked(&port->write_fifo,
				&ptr[F81534_RECEIVE_BLOCK_SIZE * port_num + 4],
				F81534_MAX_TX_SIZE, &port->lock);

	return F81534_WRITE_BUFFER_SIZE;
}

static int f81534_submit_writer(struct usb_serial_port *port, gfp_t mem_flags)
{
	struct usb_serial *serial = port->serial;
	struct f81534_port_private *port_priv = usb_get_serial_port_data(port);
	struct f81534_serial_private *serial_priv =
			usb_get_serial_data(serial);
	struct tty_struct *tty;
	struct urb *urb;
	bool cts_status = true;
	unsigned long flags;
	int result;

	tty = tty_port_tty_get(&port->port);
	if (!tty)
		return 0;

	/* check H/W Flow status */
	if (C_CRTSCTS(tty)) {
		spin_lock_irqsave(&port_priv->msr_lock, flags);
		cts_status = !!(port_priv->shadow_msr & UART_MSR_CTS);
		spin_unlock_irqrestore(&port_priv->msr_lock, flags);
	}

	tty_kref_put(tty);

	if (!cts_status)
		return 0;

	/* check is any data in write_fifo */
	spin_lock_irqsave(&port->lock, flags);

	if (kfifo_is_empty(&port->write_fifo)) {
		spin_unlock_irqrestore(&port->lock, flags);
		return 0;
	}

	spin_unlock_irqrestore(&port->lock, flags);

	/* check H/W is TXEMPTY */
	spin_lock_irqsave(&serial_priv->tx_empty_lock, flags);

	if (serial_priv->is_phy_port_not_empty[port_priv->phy]) {
		spin_unlock_irqrestore(&serial_priv->tx_empty_lock, flags);
		return 0;
	}

	serial_priv->is_phy_port_not_empty[port_priv->phy] = true;
	spin_unlock_irqrestore(&serial_priv->tx_empty_lock, flags);

	urb = port->write_urbs[0];
	f81534_prepare_write_buffer(port, port->bulk_out_buffers[0],
					port->bulk_out_size);
	urb->transfer_buffer_length = F81534_WRITE_BUFFER_SIZE;

	result = usb_submit_urb(urb, mem_flags);
	if (result) {
		dev_err(&port->dev, "%s: submit failed: %d\n", __func__,
				result);
		return result;
	}

	return 0;
}

static int f81534_switch_gpio_mode(struct usb_serial_port *port, u8 mode)
{
	struct f81534_port_private *port_priv = usb_get_serial_port_data(port);
	struct usb_device *dev = port->serial->dev;
	int x = port_priv->phy, val;
	size_t y;
	int status;
	const struct reg_value *ptr;

	if (mode > F81534_PIN_SET_MAX || x >= F81534_NUM_PORT)
		return -EINVAL;

	ptr = f81534_pin_control[x];

	for (y = 0; y < ARRAY_SIZE(f81534_pin_control[x]); ++y) {
		val = mode & BIT(y) ? 0xff : 0;
		status = f81534_set_mask_normal_register(dev,
					ptr[y].reg_address,
					BIT(ptr[y].reg_offset), val);
		if (status) {
			dev_err(&port->dev, "%s: index: %zu failed: %d\n",
					__func__, y, status);
			return status;
		}
	}

	return 0;
}

/*
 * This function will be executed when
 *	1. Port configuration change. (e.g., UART/GPIO Mode changed)
 *	2. Old IC or configuration detected.
 *         During the port probe(), We'll check the current port is final port.
 *	   If we found a old style configuration value, the
 *	   f81534_load_configure_data() will transform old to new default
 *	   setting to RAM, then f81534_save_configure_data() will compare the
 *	   flash & RAM setting, If not the same, write it with new data with
 *	   final port probe().
 */
static int f81534_save_configure_data(struct usb_serial_port *port)
{
	int status;
	size_t count;
	int phy;
	int gpio_address, uart_address;
	bool reconfigure = false;
	u8 uart_mode, gpio_mode;
	u8 data[F81534_DEF_CONF_SIZE + 1];
	u8 tmp[F81534_DEF_CONF_SIZE];
	enum f81534_uart_mode current_mode;
	struct usb_serial *serial = port->serial;
	struct f81534_port_private *port_priv;
	struct f81534_serial_private *serial_priv =
			usb_get_serial_data(serial);

	/* compare memory with ic data */
	for (count = 0; count < serial->num_ports; ++count) {
		port_priv = usb_get_serial_port_data(serial->port[count]);
		if (!port_priv) {
			dev_err(&port->dev, "%s: port_priv == NULL\n",
					__func__);
			continue;
		}

		phy = port_priv->phy;

		if (serial_priv->setting_idx == F81534_CUSTOM_NO_CUSTOM_DATA) {
			uart_address = F81534_DEF_CONF_ADDRESS_START + phy;
			gpio_address = F81534_DEF_CONF_ADDRESS_START + phy +
							F81534_CONF_SIZE;
		} else {
			uart_address = F81534_CUSTOM_ADDRESS_START + phy +
				F81534_CONF_OFFSET;

			gpio_address = F81534_CUSTOM_ADDRESS_START + phy +
				F81534_CONF_SIZE + F81534_CONF_OFFSET;
		}

		status = f81534_read_data(port->serial, uart_address, 1,
						&uart_mode);
		if (status) {
			dev_err(&port->dev, "%s: read mode failed: %d\n",
					__func__, status);
			return status;
		}

		status = f81534_read_data(port->serial, gpio_address, 1,
						&gpio_mode);
		if (status) {
			dev_err(&port->dev, "%s: read gpio failed: %d\n",
					__func__, status);
			return status;
		}

		if (port_priv->gpio_mode != gpio_mode)
			reconfigure = true;

		/* check uart flag */
		if (port_priv->uart_mode == uart_mode_rs232) {
			if ((uart_mode & F81534_MODE_MASK) !=
					F81534_RS232_FLAG)
				reconfigure = true;
		} else if (port_priv->uart_mode == uart_mode_rs485_1) {
			if ((uart_mode & F81534_MODE_MASK) !=
					F81534_RS485_1_FLAG)
				reconfigure = true;
		} else if (port_priv->uart_mode == uart_mode_rs485) {
			if ((uart_mode & F81534_MODE_MASK) !=
					F81534_RS485_FLAG)
				reconfigure = true;
		} else {
			reconfigure = true;
		}

		if (reconfigure)
			break;
	}

	if (serial_priv->setting_idx == F81534_CUSTOM_NO_CUSTOM_DATA) {
		dev_info(&serial->dev->dev, "%s: force to reconfigure\n",
					__func__);
	} else if (!reconfigure) {
		dev_dbg(&serial->dev->dev, "%s: update-to-date\n", __func__);
		return 0;
	}

	dev_info(&serial->dev->dev, "%s: updating\n", __func__);

	/* save data with setting block0 */
	serial_priv->setting_idx = 0;
	dev_info(&serial->dev->dev, "%s: save to block index: %d\n", __func__,
			serial_priv->setting_idx);

	/* erase */
	status = f81534_erase_sector(serial, F81534_CUSTOM_ADDRESS_START);
	if (status) {
		dev_err(&port->dev, "%s: erase failed: %d\n", __func__,
				status);
		return status;
	}

	/* re-check configurations */
	for (count = 0; count < serial->num_ports; ++count) {
		port_priv = usb_get_serial_port_data(serial->port[count]);
		phy = port_priv->phy;
		current_mode = port_priv->uart_mode;
		gpio_mode = port_priv->gpio_mode;

		serial_priv->default_conf_data[phy + F81534_CONF_SIZE] =
								gpio_mode;
		serial_priv->default_conf_data[phy] &= ~(F81534_MODE_MASK);

		/* check uart flag */
		if (current_mode == uart_mode_rs232) {
			serial_priv->default_conf_data[phy] |=
					F81534_RS232_FLAG;
		} else if (current_mode == uart_mode_rs485_1) {
			serial_priv->default_conf_data[phy] |=
					F81534_RS485_1_FLAG;
		} else if (current_mode == uart_mode_rs485) {
			serial_priv->default_conf_data[phy] |=
					F81534_RS485_FLAG;
		} else {
			dev_err(&serial->dev->dev,
					"%s: current_mode error, value: %d\n",
					__func__, current_mode);
		}

		dev_info(&serial->dev->dev,
				"%s: port: %zu uart_mode: %x, gpio_mode: %x\n",
				__func__, count,
				serial_priv->default_conf_data[phy + 0],
				gpio_mode);
	}

	/*
	 * 1st byte is a indicator, 0xff is empty,
	 * F81534_CUSTOM_VALID_TOKEN(0xf0) is had data only write 8 bytes of
	 * total 4 port uart & gpio mode so we need write 1+8 data
	 */

	/* token of data exist */
	data[0] = F81534_CUSTOM_VALID_TOKEN;
	memcpy(&data[1], serial_priv->default_conf_data, F81534_DEF_CONF_SIZE);

	status = f81534_write_data(serial, F81534_CUSTOM_ADDRESS_START,
			sizeof(data), data);
	if (status) {
		dev_err(&port->dev, "%s: write data failed: %d\n", __func__,
				status);
		return status;
	}

	/* recheck internal storage & memory data consistency */
	memset(tmp, 0, sizeof(tmp));

	status = f81534_read_data(serial, F81534_CUSTOM_ADDRESS_START +
				F81534_CONF_OFFSET, sizeof(tmp), tmp);
	if (status) {
		dev_err(&port->dev, "%s: read data failed: %d\n", __func__,
				status);
		return status;
	}

	for (count = 0; count < F81534_DEF_CONF_SIZE; ++count) {
		if (tmp[count] == serial_priv->default_conf_data[count])
			continue;

		dev_err(&port->dev,
				"%s: confirm error, count: %zu, data: %x %x\n",
				__func__, count, tmp[count],
				serial_priv->default_conf_data[count]);
	}

	dev_dbg(&serial->dev->dev, "%s: complete\n", __func__);
	return 0;
}

static u32 f81534_calc_baud_divisor(u32 baudrate, u32 clockrate, u32 *remain)
{
	u32 rem;

	if (!baudrate)
		return 0;

	rem = clockrate % baudrate;

	if (remain)
		*remain = rem;

	/* Round to nearest divisor */
	return DIV_ROUND_CLOSEST(clockrate, baudrate);
}

static int f81534_set_port_config(struct usb_device *dev, u8 port_number,
					 struct usb_serial_port *port,
					 u32 baudrate, u16 lcr)
{
	struct usb_serial *serial = port->serial;
	struct f81534_port_private *port_priv = usb_get_serial_port_data(port);
	u16 device_port = port_priv->phy;
	u32 divisor, rem, baud_base;
	int status;
	u8 value;
	size_t count;
	bool is_485_mode = false;
	bool is_need_invert = false;
	static u32 const baudrate_table[3] = { 1500000, 1152000, 921600};
	static u8 const clock_table[3] = { CLKSEL_24_MHZ, CLKSEL_18DOT46_MHZ,
						CLKSEL_14DOT77_MHZ};

	if (port_priv->uart_mode >= uart_mode_invalid)
		return -EINVAL;

	switch (port_priv->uart_mode) {
	case uart_mode_rs232:
		break;
	case uart_mode_rs485:
		is_need_invert = true;
	default:
		is_485_mode = true;
		break;
	}

	/*
	 * This device clock rate is configured by CLK_SEL_REGISTER bit 2~1.
	 * The following list is the list of clock rate.
	 *
	 * bit 2~1 = 00: 1.8461 MHz
	 * bit 2~1 = 01: 18.461 MHz
	 * bit 2~1 = 10: 24 MHz
	 * bit 2~1 = 11: 14.77 MHz
	 * bit 0: UART EN, must be 1
	 */
	if (baudrate <= 115200) {
		/* clock rate fixed 1.8461 MHz when below or equal 115200bps */
		value = CLKSEL_1DOT846_MHZ;
		divisor = f81534_calc_baud_divisor(baudrate, 115200, NULL);
		port_priv->current_baud_base = 115200;
	} else {
		/* Higher than 115200bps need calculate suitable clock rate */
		for (count = 0; count < ARRAY_SIZE(baudrate_table) ; ++count) {
			baud_base = baudrate_table[count];
			divisor = f81534_calc_baud_divisor(baudrate, baud_base,
								&rem);
			if (!rem) {
				dev_dbg(&port->dev, "%s: found clockbase %d\n",
						__func__,
						baudrate_table[count]);
				value = clock_table[count];
				port_priv->current_baud_base = baud_base;
				break;
			}
		}

		if (count >= ARRAY_SIZE(baudrate_table)) {
			dev_err(&port->dev,
					"%s: cant find suitable clockbase\n",
					__func__);
			return -EINVAL;
		}
	}

	value &= ~(F81534_RS485_MODE | F81534_RS485_INVERT);
	value |= is_485_mode ? F81534_RS485_MODE : 0;
	value |= is_need_invert ? F81534_RS485_INVERT : 0;

	status = f81534_setregister(serial->dev, device_port, CLK_SEL_REGISTER,
					value);
	if (status) {
		dev_err(&port->dev, "%s: CLK REG setting failed.\n", __func__);
		return status;
	}

	if (baudrate <= 1200)
		value = F81534_1X_RXTRIGGER;	/* 128 FIFO & TL: 1x */
	else
		value = F81534_8X_RXTRIGGER;	/* 128 FIFO & TL: 8x */

	status = f81534_setregister(serial->dev, device_port, CONFIG1_REGISTER,
					value);
	if (status) {
		dev_err(&port->dev, "%s: CONFIG1 setting failed.\n", __func__);
		return status;
	}

	if (baudrate <= 1200)
		value = UART_FCR_TRIGGER_1 | UART_FCR_ENABLE_FIFO; /* TL: 1 */
	else if (baudrate >= 1152000)
		value = UART_FCR_R_TRIG_10 | UART_FCR_ENABLE_FIFO; /* TL: 8 */
	else
		value = UART_FCR_R_TRIG_11 | UART_FCR_ENABLE_FIFO; /* TL: 14 */

	status = f81534_setregister(serial->dev, device_port,
					    FIFO_CONTROL_REGISTER, value);
	if (status) {
		dev_err(&port->dev, "%s: FCR setting failed.\n", __func__);
		return status;
	}

	value = UART_LCR_DLAB;
	status = f81534_setregister(serial->dev, device_port,
						LINE_CONTROL_REGISTER, value);
	if (status) {
		dev_err(&port->dev, "%s: set LCR failed.\n", __func__);
		return status;
	}

	value = divisor & 0xff;
	status = f81534_setregister(serial->dev, device_port,
					DIVISOR_LATCH_LSB, value);
	if (status) {
		dev_err(&port->dev, "%s: set DLAB LSB failed.\n", __func__);
		return status;
	}

	value = (divisor >> 8) & 0xff;
	status = f81534_setregister(serial->dev, device_port,
					DIVISOR_LATCH_MSB, value);
	if (status) {
		dev_err(&port->dev, "%s: set DLAB MSB failed.\n", __func__);
		return status;
	}

	status = f81534_setregister(serial->dev, device_port,
						LINE_CONTROL_REGISTER, lcr);
	if (status) {
		dev_err(&port->dev, "%s: set LCR failed.\n", __func__);
		return status;
	}

	return 0;
}

static int f81534_update_mctrl(struct usb_serial_port *port, unsigned int set,
				unsigned int clear)
{
	struct usb_device *dev = port->serial->dev;
	struct f81534_port_private *port_priv = usb_get_serial_port_data(port);
	u8 tmp;
	int status;

	mutex_lock(&port_priv->msr_mutex);

	if (((set | clear) & (TIOCM_DTR | TIOCM_RTS)) == 0) {
		dev_dbg(&dev->dev, "%s: DTR|RTS not being set/cleared\n",
				__func__);
		mutex_unlock(&port_priv->msr_mutex);
		return 0;	/* no change */
	}

	/* 'set' takes precedence over 'clear' */
	clear &= ~set;

	/* always enable UART_MCR_OUT2 */
	tmp = UART_MCR_OUT2 | port_priv->shadow_mcr;

	if (clear & TIOCM_DTR) {
		tmp &= ~UART_MCR_DTR;
		dev_dbg(&dev->dev, "%s: port: %d clear DTR\n", __func__,
				port_priv->phy);
	}

	if (clear & TIOCM_RTS) {
		tmp &= ~UART_MCR_RTS;
		dev_dbg(&dev->dev, "%s: port: %d clear RTS\n", __func__,
				port_priv->phy);

	}

	if (set & TIOCM_DTR) {
		tmp |= UART_MCR_DTR;
		dev_dbg(&dev->dev, "%s: port: %d set DTR\n", __func__,
				port_priv->phy);

	}

	if (set & TIOCM_RTS) {
		tmp |= UART_MCR_RTS;
		dev_dbg(&dev->dev, "%s: port: %d set RTS\n", __func__,
				port_priv->phy);
	}

	status = f81534_setregister(dev, port_priv->phy,
					MODEM_CONTROL_REGISTER, tmp);
	if (status < 0) {
		dev_err(&port->dev, "%s: MCR write failed.\n", __func__);
		mutex_unlock(&port_priv->msr_mutex);
		return status;
	}

	port_priv->shadow_mcr = tmp;
	mutex_unlock(&port_priv->msr_mutex);
	return 0;
}

/*
 * This function will search the data area with token F81534_CUSTOM_VALID_TOKEN
 * for latest configuration index. If nothing found (*index = -1), the caller
 * will load default configure in F81534_DEF_CONF_ADDRESS_START section.
 *
 * Due to we only use block0 to save data, so *index should be 0 or
 * F81534_CUSTOM_NO_CUSTOM_DATA(-1).
 */
static int f81534_find_config_idx(struct usb_serial *serial, uintptr_t *index)
{
	u8 custom_data;
	int status;

	status = f81534_read_data(serial, F81534_CUSTOM_ADDRESS_START, 1,
				&custom_data);
	if (status) {
		dev_err(&serial->dev->dev, "%s: read failed: %d\n", __func__,
				status);
		return status;
	}

	/*
	 * If had custom setting, override. The 1st byte is a
	 * indicator. 0xff is empty, F81534_CUSTOM_VALID_TOKEN is had
	 * data. read and skip with 1st data.
	 */
	if (custom_data == F81534_CUSTOM_VALID_TOKEN)
		*index = 0;
	else
		*index = F81534_CUSTOM_NO_CUSTOM_DATA;

	return 0;
}

/*
 * We had 3 generation of F81532/534 IC. All has an internal storage.
 *
 * 1st is pure USB-to-TTL RS232 IC and designed for 4 ports only, no any
 * internal data will used. All mode and gpio control should manually set
 * by AP or Driver and all storage space value are 0xff. The
 * f81534_calc_num_ports() will run to final we marked as "oldest version"
 * for this IC.
 *
 * 2nd is designed to match our transceivers(F81437/438/439). We'll save data
 * in F81534_DEF_CONF_ADDRESS_START(0x3000) with 8bytes. The first 4bytes is
 * transceiver type, value is only 0x37/0x38/0x39 to represent F81437/438/439,
 * and the following 4bytes are save mode & gpio state, the last 4bytes will
 * be limited by the first 4bytes transceiver type. The
 * f81534_calc_num_ports() will run to "older configuration" with checking
 * F81534_OLD_CONFIG_37/F81534_OLD_CONFIG_38/F81534_OLD_CONFIG_39 section.
 *
 * 3rd is designed to more generic to use any transceiver and this is our
 * mass production type. We'll save data in F81534_CUSTOM_ADDRESS_START
 * (0x2f00) with 9bytes. The 1st byte is a indicater. If the token is not
 * F81534_CUSTOM_VALID_TOKEN(0xf0), the IC is 2nd gen type, the following
 * 4bytes save port mode (0:RS232/1:RS485 Invert/2:RS485), and the last
 * 4bytes save GPIO state(value from 0~7 to represent 3 GPIO output pin).
 * The f81534_calc_num_ports() will run to "new style" with checking
 * F81534_PORT_UNAVAILABLE section.
 */
static int f81534_calc_num_ports(struct usb_serial *serial)
{
	uintptr_t setting_idx;
	size_t i;
	u8 num_port = 0;
	int status;
	unsigned char setting[F81534_CUSTOM_DATA_SIZE];

	/* check had custom setting */
	status = f81534_find_config_idx(serial, &setting_idx);
	if (status) {
		dev_err(&serial->dev->dev, "%s: find idx failed: %d\n",
				__func__, status);
		return 0;
	}

	/* Save the configuration area idx as private data for attach() */
	usb_set_serial_data(serial, (void *)setting_idx);

	/* read default board setting */
	status = f81534_read_data(serial, F81534_DEF_CONF_ADDRESS_START,
				  F81534_NUM_PORT, setting);
	if (status) {
		dev_err(&serial->dev->dev, "%s: read failed: %d\n", __func__,
				status);
		return 0;
	}

	/*
	 * If had custom setting, override it. 1st byte is a indicator, 0xff
	 * is empty, F81534_CUSTOM_VALID_TOKEN is had data, then skip with 1st
	 * data
	 */
	if (setting_idx != F81534_CUSTOM_NO_CUSTOM_DATA) {
		status = f81534_read_data(serial, F81534_CUSTOM_ADDRESS_START +
						F81534_CONF_OFFSET,
						sizeof(setting), setting);
		if (status) {
			dev_err(&serial->dev->dev,
					"%s: get custom data failed: %d\n",
					__func__, status);
			return 0;
		}

		dev_dbg(&serial->dev->dev,
				"%s: read configure from block: %d\n",
				__func__, (unsigned int)setting_idx);
	} else {
		dev_dbg(&serial->dev->dev, "%s: read configure default\n",
				__func__);
	}

	for (i = 0; i < F81534_NUM_PORT; ++i) {
		/*
		 * For older configuration use. We'll transform it to newer
		 * setting after load it with final port probed. Please
		 * reference the comments of f81534_calc_num_ports() to get
		 * detail.
		 */
		switch (setting[i]) {
		case F81534_OLD_CONFIG_37:
		case F81534_OLD_CONFIG_38:
		case F81534_OLD_CONFIG_39:
			++num_port;
			break;
		}
	}

	if (num_port) {
		dev_dbg(&serial->dev->dev, "%s: old style with %d ports",
				__func__, num_port);
		return num_port;
	}

	/* new style, find all possible ports */
	num_port = 0;
	for (i = 0; i < F81534_NUM_PORT; ++i) {
		if (setting[i] & F81534_PORT_UNAVAILABLE)
			continue;

		++num_port;
	}

	if (num_port)
		return num_port;

	dev_warn(&serial->dev->dev, "Read Failed. default 4 ports\n");
	return 4;		/* nothing found, oldest version IC */
}

static void f81534_set_termios(struct tty_struct *tty,
				struct usb_serial_port *port,
				struct ktermios *old_termios)
{
	struct usb_device *dev = port->serial->dev;
	struct f81534_port_private *port_priv;
	u32 baud;
	u16 new_lcr = 0;
	int status;

	port_priv = usb_get_serial_port_data(port);

	if (C_BAUD(tty) == B0)
		f81534_update_mctrl(port, 0, TIOCM_DTR | TIOCM_RTS);
	else if (old_termios && (old_termios->c_cflag & CBAUD) == B0)
		f81534_update_mctrl(port, TIOCM_DTR | TIOCM_RTS, 0);

	if (C_PARENB(tty)) {
		new_lcr |= UART_LCR_PARITY;

		if (!C_PARODD(tty))
			new_lcr |= UART_LCR_EPAR;

		if (C_CMSPAR(tty))
			new_lcr |= UART_LCR_SPAR;
	}

	if (C_CSTOPB(tty))
		new_lcr |= UART_LCR_STOP;

	switch (C_CSIZE(tty)) {
	case CS5:
		new_lcr |= UART_LCR_WLEN5;
		break;
	case CS6:
		new_lcr |= UART_LCR_WLEN6;
		break;
	case CS7:
		new_lcr |= UART_LCR_WLEN7;
		break;
	default:
	case CS8:
		new_lcr |= UART_LCR_WLEN8;
		break;
	}

	baud = tty_get_baud_rate(tty);

	if (baud) {
		/* Our device is not support for 1Mbps baudrate */
		if ((baud == 1000000) || (baud > F81534_MAX_BAUDRATE)) {
			if (old_termios)
				baud = old_termios->c_ospeed;
			else
				baud = F81534_DEFAULT_BAUD_RATE;
		}

		dev_dbg(&dev->dev, "%s: baud: %d\n", __func__, baud);
		tty_encode_baud_rate(tty, baud, baud);
		port_priv->current_baud_rate = baud;
	}

	port_priv->shadow_lcr = new_lcr;
	status = f81534_set_port_config(dev, port_priv->phy, port,
					port_priv->current_baud_rate, new_lcr);
	if (status < 0) {
		dev_err(&port->dev, "%s: set port config failed: %d\n",
				__func__, status);
	}

	/* Re-submit writer to re-check H/W flow Control */
	status = f81534_submit_writer(port, GFP_KERNEL);
	if (status) {
		dev_err(&port->dev, "%s: submit failed: %d\n", __func__,
				status);
	}
}

static int f81534_submit_read_urb(struct usb_serial *serial, gfp_t flags)
{
	int status;

	status = usb_serial_generic_submit_read_urbs(serial->port[0], flags);
	if (status) {
		dev_err(&serial->dev->dev, "%s: submit read URB failed: %d\n",
				__func__, status);
		return status;
	}

	return 0;
}

static void f81534_msr_changed(struct usb_serial_port *port, u8 msr,
				bool is_port_open)
{
	u8 old_msr;
	struct tty_struct *tty;
	struct f81534_port_private *port_priv = usb_get_serial_port_data(port);
	unsigned long flags;
	int status;

	if (!(msr & UART_MSR_ANY_DELTA))
		return;

	spin_lock_irqsave(&port_priv->msr_lock, flags);
	old_msr = port_priv->shadow_msr;
	port_priv->shadow_msr = msr;
	spin_unlock_irqrestore(&port_priv->msr_lock, flags);

	if ((msr & (UART_MSR_CTS | UART_MSR_DCTS)) ==
			(UART_MSR_CTS | UART_MSR_DCTS)) {
		/* CTS changed, wakeup writer to re-check flow control */
		if (is_port_open) {
			status = f81534_submit_writer(port, GFP_ATOMIC);
			if (status) {
				dev_err(&port->dev, "%s: submit failed\n",
						__func__);
			}
		}
		dev_dbg(&port->dev, "%s: CTS Flag changed, value: %x\n",
				__func__, !!(msr & UART_MSR_CTS));
	}

	dev_dbg(&port->dev, "%s: MSR from %02x to %02x\n", __func__, old_msr,
			msr);

	if (!is_port_open)
		return;

	/* update input line counters */
	if (msr & UART_MSR_DCTS)
		port->icount.cts++;
	if (msr & UART_MSR_DDSR)
		port->icount.dsr++;
	if (msr & UART_MSR_DDCD)
		port->icount.dcd++;
	if (msr & UART_MSR_TERI)
		port->icount.rng++;

	wake_up_interruptible(&port->port.delta_msr_wait);

	if (!(msr & UART_MSR_DDCD))
		return;

	dev_dbg(&port->dev, "%s: DCD Changed: port %d from %x to %x.\n",
			__func__, port_priv->phy, old_msr, msr);

	tty = tty_port_tty_get(&port->port);
	if (!tty)
		return;

	usb_serial_handle_dcd_change(port, tty, msr & UART_MSR_DCD);
	tty_kref_put(tty);
}

static int f81534_read_msr(struct usb_serial_port *port)
{
	int status;
	struct f81534_port_private *port_priv = usb_get_serial_port_data(port);
	struct usb_serial *serial = port->serial;
	unsigned long flags;
	int phy = port_priv->phy;
	u8 msr;

	/* Get MSR initial value*/
	status = f81534_getregister(serial->dev, phy, MODEM_STATUS_REGISTER,
					&msr);
	if (status)
		return status;

	/* force update current state */
	spin_lock_irqsave(&port_priv->msr_lock, flags);
	port_priv->shadow_msr = msr;
	spin_unlock_irqrestore(&port_priv->msr_lock, flags);

	f81534_msr_changed(port, msr, true);
	return 0;
}

static int f81534_open(struct tty_struct *tty, struct usb_serial_port *port)
{
	struct f81534_serial_private *serial_priv =
			usb_get_serial_data(port->serial);
	struct f81534_port_private *port_priv = usb_get_serial_port_data(port);
	int status, phy = port_priv->phy;

	status = f81534_setregister(port->serial->dev, phy,
				FIFO_CONTROL_REGISTER, UART_FCR_ENABLE_FIFO |
				UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
	if (status) {
		dev_err(&port->dev, "%s: Clear FIFO failed: %d\n", __func__,
				status);
		return status;
	}

	if (tty)
		f81534_set_termios(tty, port, &tty->termios);

	status = f81534_read_msr(port);
	if (status)
		return status;

	atomic_inc(&serial_priv->port_active[phy]);
	return 0;
}

static void f81534_close(struct usb_serial_port *port)
{
	size_t i;
	unsigned long flags;
	struct f81534_serial_private *serial_priv =
			usb_get_serial_data(port->serial);
	struct f81534_port_private *port_priv = usb_get_serial_port_data(port);
	int phy = port_priv->phy;

	atomic_dec(&serial_priv->port_active[phy]);

	/* referenced from usb_serial_generic_close() */
	for (i = 0; i < ARRAY_SIZE(port->write_urbs); ++i)
		usb_kill_urb(port->write_urbs[i]);

	spin_lock_irqsave(&port->lock, flags);
	kfifo_reset_out(&port->write_fifo);
	spin_unlock_irqrestore(&port->lock, flags);
}

static int f81534_get_serial_info(struct usb_serial_port *port,
				  struct serial_struct __user *retinfo)
{
	struct serial_struct tmp;
	struct f81534_port_private *port_priv;

	port_priv = usb_get_serial_port_data(port);
	if (!port_priv)
		return -EFAULT;

	if (!retinfo)
		return -EFAULT;

	memset(&tmp, 0, sizeof(tmp));

	tmp.type = PORT_16550A;
	tmp.port = port->port_number;
	tmp.line = port->minor;
	tmp.baud_base = port_priv->current_baud_base;

	if (copy_to_user(retinfo, &tmp, sizeof(*retinfo)))
		return -EFAULT;

	return 0;
}

static int f81534_ioctl(struct tty_struct *tty, unsigned int cmd,
			unsigned long arg)
{
	struct usb_serial_port *port = tty->driver_data;

	switch (cmd) {
	case TIOCGSERIAL:
		return f81534_get_serial_info(port,
						(struct serial_struct __user *)
						arg);
	default:
		break;
	}

	return -ENOIOCTLCMD;
}

static void f81534_process_per_serial_block(struct usb_serial_port *port,
		unsigned char *data)
{
	u8 lsr;
	char tty_flag;
	struct usb_serial *serial = port->serial;
	struct f81534_serial_private *serial_priv =
			usb_get_serial_data(serial);
	int status;
	int phy_port_num = data[0];
	size_t i, read_size = 0;
	unsigned long flags;
	bool available = !!atomic_read(
				&serial_priv->port_active[phy_port_num]);

	/*
	 * The block layout is 128 Bytes
	 * index 0: port phy idx (e.g., 0,1,2,3),
	 * index 1: It's could be
	 *			F81534_TOKEN_RECEIVE
	 *			F81534_TOKEN_TX_EMPTY
	 *			F81534_TOKEN_MSR_CHANGE
	 * index 2: serial in size (data+lsr, must be even)
	 *			meaningful for F81534_TOKEN_RECEIVE only
	 * index 3: current MSR with this device
	 * index 4~127: serial in data block (data+lsr, must be even)
	 */
	switch (data[1]) {
	case F81534_TOKEN_TX_EMPTY:
		/*
		 * We should save TX_EMPTY flag even the port is not opened
		 */
		spin_lock_irqsave(&serial_priv->tx_empty_lock, flags);
		serial_priv->is_phy_port_not_empty[phy_port_num] = false;
		spin_unlock_irqrestore(&serial_priv->tx_empty_lock, flags);
		usb_serial_port_softint(port);

		if (!available)
			return;

		/* try to submit writer only when port is opened */
		status = f81534_submit_writer(port, GFP_ATOMIC);
		if (status)
			dev_err(&port->dev, "%s: submit failed\n", __func__);
		return;

	case F81534_TOKEN_MSR_CHANGE:
		/*
		 * We'll save MSR value when device reported even when port
		 * is not opened. If the port is not opened, the MSR will only
		 * recorded without any future process.
		 */
		f81534_msr_changed(port, data[3], available);
		return;

	case F81534_TOKEN_RECEIVE:
		if (!available)
			return;

		read_size = data[2];
		break;

	default:
		dev_warn(&port->dev, "%s: unknown token:%02x\n", __func__,
				data[1]);
		return;
	}

	for (i = 4; i < 4 + read_size; i += 2) {
		tty_flag = TTY_NORMAL;
		lsr = data[i + 1];

		if (lsr & UART_LSR_BRK_ERROR_BITS) {
			if (lsr & UART_LSR_BI) {
				tty_flag = TTY_BREAK;
				port->icount.brk++;
				usb_serial_handle_break(port);
			} else if (lsr & UART_LSR_PE) {
				tty_flag = TTY_PARITY;
				port->icount.parity++;
			} else if (lsr & UART_LSR_FE) {
				tty_flag = TTY_FRAME;
				port->icount.frame++;
			}

			if (lsr & UART_LSR_OE) {
				port->icount.overrun++;
				tty_insert_flip_char(&port->port, 0,
						TTY_OVERRUN);
			}
		}

		if (port->port.console && port->sysrq) {
			if (usb_serial_handle_sysrq_char(port, data[i]))
				continue;
		}

		tty_insert_flip_char(&port->port, data[i], tty_flag);
	}

	tty_flip_buffer_push(&port->port);
}

static void f81534_process_read_urb(struct urb *urb)
{
	size_t i;
	int phy_port_num;
	int tty_port_num;
	unsigned char *ch;
	struct usb_serial *serial;
	struct usb_serial_port *port;
	struct f81534_port_private *port_priv;

	if (!urb->actual_length)
		return;

	port = urb->context;
	serial = port->serial;
	ch = urb->transfer_buffer;

	for (i = 0; i < urb->actual_length; i += F81534_RECEIVE_BLOCK_SIZE) {
		phy_port_num = ch[i];
		tty_port_num = f81534_phy_to_logic_port(serial, phy_port_num);
		port = serial->port[tty_port_num];

		/*
		 * The device will send back all information when we submitted
		 * a read URB (MSR/DATA/TX_EMPTY). But it maybe get callback
		 * before port_probe() or after port_remove().
		 *
		 * So we'll verify the pointer. If the pointer is NULL, it's
		 * mean the port not init complete and the block will skip.
		 */
		port_priv = usb_get_serial_port_data(port);
		if (!port_priv) {
			dev_warn(&serial->dev->dev,
					"%s: phy: %d not ready\n", __func__,
					phy_port_num);
			continue;
		}

		f81534_process_per_serial_block(port, &ch[i]);
	}
}

static void f81534_write_usb_callback(struct urb *urb)
{
	struct usb_serial_port *port = urb->context;
	int status = urb->status;

	if (status) {
		dev_warn(&port->dev, "%s: non-zero URB status: %d\n", __func__,
				status);
	} else {
		usb_serial_port_softint(port);
	}
}

static int f81534_setup_ports(struct usb_serial *serial)
{
	size_t i, j;
	u8 port0_out_address;
	int buffer_size;
	struct usb_serial_port *port;

	/*
	 * In our system architecture, we had 2 or 4 serial ports,
	 * but only get 1 set of bulk in/out endpoints.
	 *
	 * The usb-serial subsystem will generate port 0 data,
	 * but port 1/2/3 will not. It's will generate write URB and buffer
	 * by following code and use the port0 read URB for read operation.
	 */
	for (i = 1; i < serial->num_ports; ++i) {
		port0_out_address = serial->port[0]->bulk_out_endpointAddress;
		buffer_size = serial->port[0]->bulk_out_size;
		port = serial->port[i];

		if (kfifo_alloc(&port->write_fifo, PAGE_SIZE, GFP_KERNEL))
			goto failed;

		port->bulk_out_size = buffer_size;
		port->bulk_out_endpointAddress = port0_out_address;

		for (j = 0; j < ARRAY_SIZE(port->write_urbs); ++j) {
			set_bit(j, &port->write_urbs_free);

			port->write_urbs[j] = usb_alloc_urb(0, GFP_KERNEL);
			if (!port->write_urbs[j])
				goto failed;

			port->bulk_out_buffers[j] = kmalloc(buffer_size,
								GFP_KERNEL);
			if (!port->bulk_out_buffers[j])
				goto failed;

			usb_fill_bulk_urb(port->write_urbs[j], serial->dev,
					usb_sndbulkpipe(serial->dev,
						port0_out_address),
					port->bulk_out_buffers[j], buffer_size,
					serial->type->write_bulk_callback,
					port);
		}

		port->write_urb = port->write_urbs[0];
		port->bulk_out_buffer = port->bulk_out_buffers[0];
	}

	return 0;
failed:
	return -ENOMEM;
}

static int f81534_load_configure_data(struct usb_serial_port *port)
{
	int status;
	int offset;
	u8 uart_flag, gpio_mode;
	struct f81534_port_private *port_priv = usb_get_serial_port_data(port);
	struct f81534_serial_private *serial_priv =
			usb_get_serial_data(port->serial);
	int device_port = port_priv->phy;

	uart_flag = serial_priv->default_conf_data[device_port];
	gpio_mode = serial_priv->default_conf_data[device_port +
							F81534_CONF_SIZE];

	switch (uart_flag) {
	/*
	 * For older configuration use. We'll transform it to newer setting
	 * after load it with final port probed. Please reference the comments
	 * of f81534_calc_num_ports() to get detail.
	 */
	case F81534_OLD_CONFIG_37:
	case F81534_OLD_CONFIG_38:
	case F81534_OLD_CONFIG_39:
		offset = device_port + F81534_CONF_SIZE;
		serial_priv->default_conf_data[device_port] =
							F81534_RS232_FLAG;
		gpio_mode = serial_priv->default_conf_data[offset] =
							F81534_PIN_SET_DEFAULT;
		port_priv->uart_mode = uart_mode_rs232;
		port_priv->gpio_mode = F81534_PIN_SET_DEFAULT;
		dev_info(&port->dev, "Internal config need to upgrade\n");
		break;

	/* MP style setting */
	default:
		if (uart_flag & F81534_PORT_CONF_RS485) {
			if (uart_flag & F81534_PORT_CONF_RS485_INVERT)
				port_priv->uart_mode = uart_mode_rs485;
			else
				port_priv->uart_mode = uart_mode_rs485_1;
		} else {
			port_priv->uart_mode = uart_mode_rs232;
		}

		break;
	}

	if ((gpio_mode >= F81534_PIN_SET_MIN) &&
			(gpio_mode <= F81534_PIN_SET_MAX)) {
		port_priv->gpio_mode = gpio_mode;
		dev_dbg(&port->dev, "gpio set to %d\n", gpio_mode);
	} else {
		port_priv->gpio_mode = F81534_PIN_SET_DEFAULT;
		dev_info(&port->dev, "unknown gpio %d, setting to %d\n",
				gpio_mode, F81534_PIN_SET_DEFAULT);
	}

	status = f81534_switch_gpio_mode(port, port_priv->gpio_mode);
	if (status) {
		dev_err(&port->dev,
				"%s: switch gpio mode failed: %d\n", __func__,
				status);
		return status;
	}

	return 0;
}

static void f81534_dump_configure(struct usb_serial *serial)
{
	unsigned char transceiver, mode;
	size_t count;
	u32 index;
	int gpio_address, uart_address;
	struct f81534_serial_private *serial_priv =
			usb_get_serial_data(serial);

	index = serial_priv->setting_idx;

	for (count = 0; count < F81534_NUM_PORT; ++count) {
		if (index == F81534_CUSTOM_NO_CUSTOM_DATA) {
			uart_address = F81534_DEF_CONF_ADDRESS_START + count;
			gpio_address = F81534_DEF_CONF_ADDRESS_START + count +
						F81534_CONF_SIZE;
		} else {
			/*
			 * If had custom setting, override. The 1st byte is a
			 * indicator. 0xff is empty, F81534_CUSTOM_VALID_TOKEN
			 * is had data. read and skip with 1st data.
			 */
			uart_address = F81534_CUSTOM_ADDRESS_START + count +
						F81534_CONF_OFFSET;

			gpio_address = F81534_CUSTOM_ADDRESS_START + count +
						F81534_CONF_SIZE +
						F81534_CONF_OFFSET;
		}

		f81534_read_data(serial, uart_address, 1, &transceiver);
		f81534_read_data(serial, gpio_address, 1, &mode);

		dev_dbg(&serial->dev->dev,
				"%s: port: %zu uart_flag: %x gpio: %x\n",
				__func__, count, transceiver, mode);
	}
}

static int f81534_attach(struct usb_serial *serial)
{
	struct f81534_serial_private *serial_priv;
	int status;
	size_t i;
	uintptr_t setting_idx = (uintptr_t)usb_get_serial_data(serial);

	serial_priv = kzalloc(sizeof(*serial_priv), GFP_KERNEL);
	if (!serial_priv)
		return -ENOMEM;

	usb_set_serial_data(serial, serial_priv);
	serial_priv->setting_idx = setting_idx;

	for (i = 0; i < F81534_NUM_PORT; ++i)
		atomic_set(&serial_priv->port_active[i], 0);

	spin_lock_init(&serial_priv->tx_empty_lock);

	status = f81534_setup_ports(serial);
	if (status)
		goto failed;

	/*
	 * The default configuration layout:
	 *	byte 0/1/2/3: uart setting
	 *	byte 4/5/6/7: gpio setting
	 *
	 * We can reference from f81534_load_configure_data().
	 */
	status = f81534_read_data(serial, F81534_DEF_CONF_ADDRESS_START,
				F81534_DEF_CONF_SIZE,
				serial_priv->default_conf_data);
	if (status) {
		dev_err(&serial->dev->dev, "%s: read reserve data failed\n",
				__func__);
		goto failed;
	}

	/*
	 * if serial_priv->setting_idx == F81534_CUSTOM_NO_CUSTOM_DATA
	 * it's mean for no configuration in custom section, so we'll use
	 * default config read from F81534_DEF_CONF_ADDRESS_START
	 */
	if (serial_priv->setting_idx == F81534_CUSTOM_NO_CUSTOM_DATA)
		return 0;

	/* only read 8 bytes for mode & GPIO */
	status = f81534_read_data(serial, F81534_CUSTOM_ADDRESS_START +
					F81534_CONF_OFFSET,
					sizeof(serial_priv->default_conf_data),
					serial_priv->default_conf_data);
	if (status) {
		dev_err(&serial->dev->dev,
				"%s: idx: %d get data failed: %d\n", __func__,
				serial_priv->setting_idx, status);
		goto failed;
	}

	/*
	 * We'll register port 0 bulkin only once, It'll take all port received
	 * data, MSR register change and TX_EMPTY information.
	 */
	status = f81534_submit_read_urb(serial, GFP_KERNEL);
	if (status)
		goto failed;

	return 0;
failed:
	kfree(serial_priv);
	return status;
}

static void f81534_release(struct usb_serial *serial)
{
	struct f81534_serial_private *serial_priv =
			usb_get_serial_data(serial);

	kfree(serial_priv);
}

static int f81534_port_probe(struct usb_serial_port *port)
{
	struct usb_serial *serial = port->serial;
	struct f81534_serial_private *serial_priv =
			usb_get_serial_data(serial);
	struct f81534_port_private *port_priv;
	size_t i, count = 0;
	int status, port_index = f81534_port_index(port);

	port_priv = kzalloc(sizeof(*port_priv), GFP_KERNEL);
	if (!port_priv)
		return -ENOMEM;

	spin_lock_init(&port_priv->msr_lock);
	mutex_init(&port_priv->msr_mutex);

	/* assign logic-to-phy mapping */
	port_priv->phy = F81534_UNUSED_PORT;

	for (i = 0; i < F81534_NUM_PORT; ++i) {
		if (serial_priv->default_conf_data[i] &
				F81534_PORT_UNAVAILABLE)
			continue;

		if (port_index == count) {
			port_priv->phy = i;
			break;
		}

		++count;
	}

	if (port_priv->phy == F81534_UNUSED_PORT) {
		status = -ENODEV;
		goto port_fail;
	}

	usb_set_serial_port_data(port, port_priv);
	dev_dbg(&port->dev, "%s: mapping to phy: %d\n", __func__,
			port_priv->phy);

	status = f81534_load_configure_data(port);
	if (status)
		goto port_fail;

	/*
	 * Driver will compare memory & flash configure. If it not the same,
	 * We'll save it when final port probed.
	 */
	if (serial->num_ports - 1 == f81534_port_index(port)) {
		f81534_save_configure_data(port);
		f81534_dump_configure(serial);
	}

	return 0;

port_fail:
	dev_err(&port->dev, "%s: failed: %d\n", __func__, status);
	kfree(port_priv);
	return status;
}

static int f81534_port_remove(struct usb_serial_port *port)
{
	size_t i;
	struct f81534_port_private *port_priv;

	/*
	 * We had only submit port0 read URB for use, but we'll kill all port
	 * read URBs for code consistency
	 */
	for (i = 0; i < ARRAY_SIZE(port->read_urbs); ++i)
		usb_kill_urb(port->read_urbs[i]);

	port_priv = usb_get_serial_port_data(port);
	kfree(port_priv);
	return 0;
}

static int f81534_tiocmget(struct tty_struct *tty)
{
	struct usb_serial_port *port = tty->driver_data;
	struct f81534_port_private *port_priv = usb_get_serial_port_data(port);
	unsigned long flags;
	int r;
	u8 msr, mcr;

	/*
	 * We'll avoid to direct read MSR register without open(). The IC will
	 * read the MSR changed and report it f81534_process_per_serial_block()
	 * by F81534_TOKEN_MSR_CHANGE.
	 *
	 * When this device in heavy loading (e.g., BurnInTest Loopback Test)
	 * The report of MSR register will delay received a bit. It's due to
	 * MSR interrupt is lowest priority in 16550A. So we decide to sleep
	 * a little time to pass the test.
	 */
	r = schedule_timeout_killable(msecs_to_jiffies(F81534_DELAY_READ_MSR));
	if (r)
		return -EINTR;

	mutex_lock(&port_priv->msr_mutex);
	spin_lock_irqsave(&port_priv->msr_lock, flags);

	msr = port_priv->shadow_msr;
	mcr = port_priv->shadow_mcr;

	spin_unlock_irqrestore(&port_priv->msr_lock, flags);
	mutex_unlock(&port_priv->msr_mutex);

	r = (mcr & UART_MCR_DTR ? TIOCM_DTR : 0) |
	    (mcr & UART_MCR_RTS ? TIOCM_RTS : 0) |
	    (msr & UART_MSR_CTS ? TIOCM_CTS : 0) |
	    (msr & UART_MSR_DCD ? TIOCM_CAR : 0) |
	    (msr & UART_MSR_RI ? TIOCM_RI : 0) |
	    (msr & UART_MSR_DSR ? TIOCM_DSR : 0);

	return r;
}

static int f81534_tiocmset(struct tty_struct *tty,
			   unsigned int set, unsigned int clear)
{
	struct usb_serial_port *port = tty->driver_data;

	return f81534_update_mctrl(port, set, clear);
}

static void f81534_dtr_rts(struct usb_serial_port *port, int on)
{
	if (on)
		f81534_update_mctrl(port, TIOCM_DTR | TIOCM_RTS, 0);
	else
		f81534_update_mctrl(port, 0, TIOCM_DTR | TIOCM_RTS);
}

static int f81534_write(struct tty_struct *tty,
			struct usb_serial_port *port,
			const unsigned char *buf, int count)
{
	int bytes_out, status;

	if (!count)
		return 0;

	bytes_out = kfifo_in_locked(&port->write_fifo, buf, count,
					&port->lock);

	status = f81534_submit_writer(port, GFP_ATOMIC);
	if (status) {
		dev_err(&port->dev, "%s: submit failed\n", __func__);
		return status;
	}

	return bytes_out;
}

static int f81534_resume(struct usb_serial *serial)
{
	struct usb_serial_port *port;
	int status, error = 0;
	size_t i;

	/*
	 * We'll register port 0 bulkin only once, It'll take all port received
	 * data, MSR register change and TX_EMPTY information.
	 */
	status = f81534_submit_read_urb(serial, GFP_NOIO);
	if (status)
		return status;

	for (i = 0; i < serial->num_ports; i++) {
		port = serial->port[i];
		if (!test_bit(ASYNCB_INITIALIZED, &port->port.flags))
			continue;

		status = f81534_submit_writer(port, GFP_NOIO);
		if (status) {
			dev_err(&port->dev, "%s: submit failed\n", __func__);
			++error;
		}
	}

	return error ? -EIO : 0;
}

static struct usb_serial_driver f81534_device = {
	.driver = {
		   .owner = THIS_MODULE,
		   .name = IC_NAME,
		   },
	.description = DRIVER_DESC,
	.id_table = f81534_id_table,
	.open = f81534_open,
	.close = f81534_close,
	.write = f81534_write,
	.calc_num_ports = f81534_calc_num_ports,
	.attach = f81534_attach,
	.release = f81534_release,
	.port_probe = f81534_port_probe,
	.port_remove = f81534_port_remove,
	.dtr_rts = f81534_dtr_rts,
	.process_read_urb = f81534_process_read_urb,
	.ioctl = f81534_ioctl,
	.tiocmget = f81534_tiocmget,
	.tiocmset = f81534_tiocmset,
	.write_bulk_callback = f81534_write_usb_callback,
	.set_termios = f81534_set_termios,
	.resume = f81534_resume,
};

static struct usb_serial_driver *const serial_drivers[] = {
	&f81534_device, NULL
};

module_usb_serial_driver(serial_drivers, f81534_id_table);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("Peter Hong <Peter_Hong@fintek.com.tw>");
MODULE_AUTHOR("Tom Tsai <Tom_Tsai@fintek.com.tw>");
MODULE_LICENSE("GPL");
