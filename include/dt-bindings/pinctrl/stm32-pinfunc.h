#ifndef _DT_BINDINGS_STM32_PINFUNC_H
#define _DT_BINDINGS_STM32_PINFUNC_H

#define STM32_PINMUX(pin, mode) (((pin) << 8) | (mode))

/*  define PIN modes */
#define GPIO	0x0
#define AF0	0x1
#define AF1	0x2
#define AF2	0x3
#define AF3	0x4
#define AF4	0x5
#define AF5	0x6
#define AF6	0x7
#define AF7	0x8
#define AF8	0x9
#define AF9	0xa
#define AF10	0xb
#define AF11	0xc
#define AF12	0xd
#define AF13	0xe
#define AF14	0xf
#define AF15	0x10
#define ANALOG	0x11

/* define Pins number*/
#define FIRST_PORT	'A'
#define PIN_NO(port_name, pin_number)	((port_name - 'A') * 0x10 + pin_number)

#endif /* _DT_BINDINGS_STM32_PINFUNC_H */

