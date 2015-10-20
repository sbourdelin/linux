extern struct sdio_func *wilc1000_sdio_func;
extern struct sdio_driver wilc_bus;

#include <linux/mmc/sdio_func.h>

int wilc1000_sdio_init(void *);
void wilc1000_sdio_deinit(void *);
int wilc1000_sdio_cmd52(sdio_cmd52_t *cmd);
int wilc1000_sdio_cmd53(sdio_cmd53_t *cmd);
int wilc1000_sdio_enable_interrupt(void);
void wilc1000_sdio_disable_interrupt(void);
int wilc1000_sdio_set_max_speed(void);
int wilc1000_sdio_set_default_speed(void);

