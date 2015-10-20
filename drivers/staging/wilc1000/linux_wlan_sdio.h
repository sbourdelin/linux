#include <linux/mmc/sdio_func.h>

extern struct sdio_func *wilc1000_sdio_func;
extern struct sdio_driver wilc_bus;
extern const struct wilc1000_ops wilc1000_sdio_ops;

int wilc1000_sdio_enable_interrupt(void);
void wilc1000_sdio_disable_interrupt(void);

