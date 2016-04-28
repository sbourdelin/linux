#ifndef GPIO_MMIO_COMPAT_H
#define GPIO_MMIO_COMPAT_H

#include <linux/ioport.h>

#define ADD(_name, _func) { .compatible = _name, .data = _func }

#if IS_ENABLED(CONFIG_GPIO_CLPS711X)
int cirrus_clps711x_parse_dt(struct platform_device *pdev,
			     struct bgpio_pdata *pdata,
			     unsigned long *flags);

#define GPIO_CLPS711X_COMPATIBLE			\
	ADD("cirrus,clps711x-gpio", cirrus_clps711x_parse_dt),
#else
#define GPIO_CLPS711X_COMPATIBLE
#endif /* CONFIG_GPIO_CLPS711X */

#if IS_ENABLED(CONFIG_GPIO_GE_FPGA)
int ge_parse_dt(struct platform_device *pdev,
		struct bgpio_pdata *pdata,
		unsigned long *flags);

#define GPIO_GE_FPGA_COMPATIBLE				\
	ADD("ge,imp3a-gpio", ge_parse_dt),		\
	ADD("gef,sbc310-gpio", ge_parse_dt),		\
	ADD("gef,sbc610-gpio", ge_parse_dt),

#else
#define GPIO_GE_FPGA_COMPATIBLE
#endif /* CONFIG_CONFIG_GPIO_GE_FPGA */

#if IS_ENABLED(CONFIG_GPIO_MOXART)
int moxart_parse_dt(struct platform_device *pdev,
		    struct bgpio_pdata *pdata,
		    unsigned long *flags);

#define GPIO_MOXART_COMPATIBLE				\
	ADD("moxa,moxart-gpio", moxart_parse_dt),

#else
#define GPIO_MOXART_COMPATIBLE
#endif /* CONFIG_GPIO_MOXART */

#if IS_ENABLED(CONFIG_GPIO_TS4800)
int technologic_ts4800_parse_dt(struct platform_device *pdev,
				struct bgpio_pdata *pdata,
				unsigned long *flags);

#define GPIO_TS4800_COMPATIBLE				\
	ADD("technologic,ts4800-gpio", technologic_ts4800_parse_dt),
#else
#define GPIO_TS4800_COMPATIBLE
#endif /* CONFIG_GPIO_TS4800 */

#undef ADD

static inline void set_resource_address(struct resource *res,
					resource_size_t start,
					resource_size_t len)
{
	res->start = start;
	res->end = start + len - 1;
}
#endif /* GPIO_MMIO_COMPAT_H */
