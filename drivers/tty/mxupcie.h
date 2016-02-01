#ifndef _MXUPCIE_H
#define _MXUPCIE_H

/* The definition of linear baud rate */
#define FREQUENCY       14745600
#define MAXDIVISOR      255
#define MAXSEQUENCE     46
#define MINSEQUENCE     4
#define MAX_SCR		12
#define MIN_SCR		0
#define MAX_CPRN	7
#define MIN_CPRN	0
#define MIN_CPRM	1
#define MAX_CPRM	2

#define MXUPCIE_BOARDS		4	/* Max. boards */
#define MXUPCIE_PORTS		32	/* Max. ports */
#define MXUPCIE_PORTS_PER_BOARD	8	/* Max. ports per board*/
#define MXUPCIE_ISR_PASS_LIMIT	99999L

#define WAKEUP_CHARS		256

#define UART_LSR_SPECIAL	0x1E

/*
 *	Define the Moxa PCI vendor and device IDs.
 */
#define	MOXA				0x1393
#define	PCI_DEVICE_ID_CP102E		0x1024
#define	PCI_DEVICE_ID_CP102EL		0x1025
#define	PCI_DEVICE_ID_CP132EL		0x1322
#define	PCI_DEVICE_ID_CP114EL		0x1144
#define	PCI_DEVICE_ID_CP104EL_A		0x1045
#define	PCI_DEVICE_ID_CP168EL_A		0x1683
#define	PCI_DEVICE_ID_CP118EL_A		0x1182
#define	PCI_DEVICE_ID_CP118E_A_I	0x1183
#define	PCI_DEVICE_ID_CP138E_A		0x1381
#define	PCI_DEVICE_ID_CP134EL_A		0x1342
#define	PCI_DEVICE_ID_CP116E_A_A	0x1160
#define	PCI_DEVICE_ID_CP116E_A_B	0x1161

#define MOXA_PUART_SFR			0x07
#define MOXA_PUART_EFR			0x0A
#define MOXA_PUART_XON1			0x0B
#define MOXA_PUART_XON2			0x0C
#define MOXA_PUART_XOFF1		0x0D
#define MOXA_PUART_XOFF2		0x0E
#define MOXA_PUART_ACR			0x0F
#define MOXA_PUART_TTL			0x10
#define MOXA_PUART_RTL			0x11
#define MOXA_PUART_FCL			0x12
#define MOXA_PUART_FCH			0x13
#define MOXA_PUART_CPR			0x14
#define MOXA_PUART_RCNT			0x15
#define MOXA_PUART_LSRCNT		0x15
#define MOXA_PUART_TCNT			0x16
#define MOXA_PUART_SCR			0x16
#define MOXA_PUART_GLSR			0x17
#define MOXA_PUART_MEMRBR		0x100
#define MOXA_PUART_MEMTHR		0x100
#define MOXA_PUART_0UIR			0x04
#define MOXA_PUART_1UIR			0x04
#define MOXA_PUART_2UIR			0x05
#define MOXA_PUART_3UIR			0x05
#define MOXA_PUART_4UIR			0x06
#define MOXA_PUART_5UIR			0x06
#define MOXA_PUART_6UIR			0x07
#define MOXA_PUART_7UIR			0x07
#define MOXA_PUART_GPIO_IN		0x08
#define MOXA_PUART_GPIO_EN		0x09
#define MOXA_PUART_GPIO_OUT		0x0A
#define MOXA_PUART_LSB			0x08
#define MOXA_PUART_MSB			0x09

#define MOXA_PUART_ADJ_CLK		0x24
#define MOXA_PUART_ADJ_ENABLE		0x25

#define MOXA_SFR_FORCE_TX		0x01
#define MOXA_SFR_950			0x20
#define MOXA_SFR_ENABLE_TCNT		0x80

#define MOXA_EFR_TX_SW			0x02
#define MOXA_EFR_RX_SW			0x08
#define MOXA_EFR_ENHANCE		0x10
#define MOXA_EFR_AUTO_RTS		0x40
#define MOXA_EFR_AUTO_CTS		0x80

#define MOXA_IIR_NO_INT			0xC1
#define MOXA_IIR_RLSI			0xC6
#define MOXA_IIR_RDI			0x04
#define MOXA_IIR_THRI			0x02

#define MOXA_TTL_1			0x01
#define MOXA_RTL_1			0x01
#define MOXA_RTL_96			0x60
#define MOXA_RTL_120			0x78
#define MOXA_FCL_16			0x10
#define MOXA_FCH_96			0x60
#define MOXA_FCH_110			0x6E
#define MOXA_FCH_120			0x78

#define MOXA_UIR_RS232			0x00
#define MOXA_UIR_RS422			0x01
#define MOXA_UIR_RS485_4W		0x0B
#define MOXA_UIR_RS485_2W		0x0F
#define MOXA_UIR_OFFSET			0x04
#define MOXA_UIR_EVEN_PORT_VALUE_OFFSET	4

#define MX_FLAG_232                   BIT(0)
#define MX_FLAG_422                   BIT(1)
#define MX_FLAG_485                   BIT(2)

#define MOXA_GPIO_SET_ALL_OUTPUT	0x0F
#define MOXA_GPIO_OUTPUT_VALUE_OFFSET	16

#define MX_TERM_NONE			0x00
#define MX_TERM_120			0x01

#define MX_PORT4			3
#define MX_PORT8			7
#define MX_TX_FIFO_SIZE			128
#define MX_RX_FIFO_SIZE			128
#define MX_PUART_SIZE			0x200
#define MX_BREAK_ON			0x01
#define MX_BREAK_OFF			0x00

#define MX_FIFO_RESET_CNT		100

#endif
