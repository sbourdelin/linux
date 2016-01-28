#ifndef __F81504_H__
#define __F81504_H__

#define FINTEK_VID			0x1c29
#define FINTEK_F81504			0x1104
#define FINTEK_F81508			0x1108
#define FINTEK_F81512			0x1112

#define FINTEK_MAX_PORT			12
#define FINTEK_GPIO_NAME_LEN		32
#define FINTEK_GPIO_DISPLAY		"GPIO"

#define F81504_UART_START_ADDR		0x40
#define F81504_UART_MODE_OFFSET		0x07
#define F81504_UART_OFFSET		0x08

/* RTS will control by MCR if this bit is 0 */
#define F81504_RTS_CONTROL_BY_HW	BIT(4)
/* only worked with FINTEK_RTS_CONTROL_BY_HW on */
#define F81504_RTS_INVERT		BIT(5)

#define F81504_CLOCK_RATE_MASK		0xc0
#define F81504_CLKSEL_1DOT846_MHZ	0x00
#define F81504_CLKSEL_18DOT46_MHZ	0x40
#define F81504_CLKSEL_24_MHZ		0x80
#define F81504_CLKSEL_14DOT77_MHZ	0xc0

#define F81504_IRQSEL_REG		0xb8

#define F81504_GPIO_ENABLE_REG		0xf0
#define F81504_GPIO_IO_LSB_REG		0xf1
#define F81504_GPIO_IO_MSB_REG		0xf2
#define F81504_GPIO_MODE_REG		0xf3

#define F81504_GPIO_START_ADDR		0xf8
#define F81504_GPIO_OUT_EN_OFFSET	0x00
#define F81504_GPIO_DRIVE_EN_OFFSET	0x01
#define F81504_GPIO_SET_OFFSET		0x08

#define F81504_GPIO_NAME		"f81504_gpio"
#define F81504_SERIAL_NAME		"f81504_serial"
#define F81504_MAX_GPIO_CNT		6

extern const u8 fintek_gpio_mapping[F81504_MAX_GPIO_CNT];

struct f81504_pci_private {
	int line[FINTEK_MAX_PORT];
	u8 gpio_en;
	u16 gpio_ioaddr;
	u32 uart_count, gpio_count;
};
#endif
