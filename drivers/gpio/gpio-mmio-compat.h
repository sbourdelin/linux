#ifndef GPIO_MMIO_COMPAT_H
#define GPIO_MMIO_COMPAT_H

#include <linux/ioport.h>

#define ADD(_name, _func) { .compatible = _name, .data = _func }

#undef ADD

static inline void set_resource_address(struct resource *res,
					resource_size_t start,
					resource_size_t len)
{
	res->start = start;
	res->end = start + len - 1;
}
#endif /* GPIO_MMIO_COMPAT_H */
