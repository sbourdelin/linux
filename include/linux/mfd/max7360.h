#ifndef __LINUX_MFD_MAX7360_H
#define __LINUX_MFD_MAX7360_H
#include <linux/regmap.h>

#define MAX7360_MAX_KEY_ROWS	8
#define MAX7360_MAX_KEY_COLS	8
#define MAX7360_MAX_KEY_NUM	(MAX7360_MAX_KEY_ROWS * MAX7360_MAX_KEY_COLS)
#define MAX7360_ROW_SHIFT	3

#define MAX7360_MAX_GPIO 8
#define MAX7360_MAX_GPO 6
#define MAX7360_COL_GPO_PINS 8
/*
 * MAX7360 registers
 */
#define MAX7360_REG_KEYFIFO	0x00
#define MAX7360_REG_CONFIG	0x01
#define MAX7360_REG_DEBOUNCE	0x02
#define MAX7360_REG_INTERRUPT	0x03
#define MAX7360_REG_PORTS	0x04
#define MAX7360_REG_KEYREP	0x05
#define MAX7360_REG_SLEEP	0x06

/*
 * MAX7360 registers
 */
#define MAX7360_REG_GPIOCFG	0x40
#define MAX7360_REG_GPIOCTRL	0x41
#define MAX7360_REG_GPIODEB	0x42
#define MAX7360_REG_GPIOCURR	0x43
#define MAX7360_REG_GPIOOUTM	0x44
#define MAX7360_REG_PWMCOM	0x45
#define MAX7360_REG_RTRCFG	0x46
#define MAX7360_REG_GPIOIN	0x49
#define MAX7360_REG_RTR_CNT	0x4A
#define MAX7360_REG_PWMBASE	0x50
#define MAX7360_REG_PWMCFG	0x58


#define MAX7360_REG_PORTCFGBASE 0x58

/*
 * Configuration register bits
 */
#define MAX7360_CFG_SLEEP	BIT(7)
#define MAX7360_CFG_INTERRUPT	BIT(5)
#define MAX7360_CFG_KEY_RELEASE	BIT(3)
#define MAX7360_CFG_WAKEUP	BIT(1)
#define MAX7360_CFG_TIMEOUT	BIT(0)

/*
 * Autosleep register values (ms)
 */
#define MAX7360_AUTOSLEEP_8192	0x01
#define MAX7360_AUTOSLEEP_4096	0x02
#define MAX7360_AUTOSLEEP_2048	0x03
#define MAX7360_AUTOSLEEP_1024	0x04
#define MAX7360_AUTOSLEEP_512	0x05
#define MAX7360_AUTOSLEEP_256	0x06

#define MAX7360_INT_INTI	0
#define MAX7360_INT_INTK	1

#define MAX7360_INT_GPIO   0
#define MAX7360_INT_KEYPAD 1
#define MAX7360_INT_ROTARY 2

#define MAX7360_NR_INTERNAL_IRQS	3

/**
 * struct max7360
 * @dev:	Parent device pointer
 * @i2c:	I2c client pointer
 * @domain:	Irq domain pointer
 * @regmap:	Used for I2C communication on accessing registers
 * @shared_irq:	Irq number if inti and intk pins are connected together
 * @irq_inti:	Irq number for inti pin
 * @irq_intk:	Irq number for inik pin
 */
struct max7360 {
	struct device		*dev;
	struct i2c_client	*i2c;
	struct irq_domain	*domain;
	struct regmap		*regmap;

	int			shared_irq;
	int			irq_inti;
	int			irq_intk;
};
#endif
