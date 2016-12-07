#ifndef __KUROBOXPRO_COMMON_H__
#define __KUROBOXPRO_COMMON_H__

#define UART1_REG(x)	(base + ((UART_##x) << 2))

int uart1_micon_send(void *base, const unsigned char *data, int count);

#endif
