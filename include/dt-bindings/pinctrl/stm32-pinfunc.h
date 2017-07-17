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
#define PA_BASE 0
#define PA(x)	((x) + PA_BASE)

#define PB_BASE 0x10
#define PB(x)	((x) + PB_BASE)

#define PC_BASE 0x20
#define PC(x)	((x) + PC_BASE)

#define PD_BASE 0x30
#define PD(x)	((x) + PD_BASE)

#define PE_BASE 0x40
#define PE(x)	((x) + PE_BASE)

#define PF_BASE 0x50
#define PF(x)	((x) + PF_BASE)

#define PG_BASE 0x60
#define PG(x)	((x) + PG_BASE)

#define PH_BASE 0x70
#define PH(x)	((x) + PH_BASE)

#define PI_BASE 0x80
#define PI(x)	((x) + PI_BASE)

#define PJ_BASE 0x90
#define PJ(x)	((x) + PJ_BASE)

#define PK_BASE 0xa0
#define PK(x)	((x) + PK_BASE)

#endif /* _DT_BINDINGS_STM32_PINFUNC_H */

