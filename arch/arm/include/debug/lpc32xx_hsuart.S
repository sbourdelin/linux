#define LPC_HSU_TX		(0x0)
#define LPC_HSU_LEVEL		(0x4)
#define LPC_HSU_LEVEL_TX	0xFF00

	.macro	addruart, rp, rv, tmp
	ldr	\rp, =CONFIG_DEBUG_UART_PHYS		@ System peripherals (phys address)
	ldr	\rv, =CONFIG_DEBUG_UART_VIRT		@ System peripherals (virt address)
	.endm

	.macro	senduart,rd,rx
	str	\rd, [\rx, #(LPC_HSU_TX)]
	.endm

	.macro	waituart,rd,rx
	.endm

	.macro	busyuart,rd,rx
1001:	ldr	\rd, [\rx, #(LPC_HSU_LEVEL)]
	ands	\rd, \rd, LPC_HSU_LEVEL_TX
	bne	1001b
	.endm

