#ifndef __ASM_MACH_EP93XX_SPI_H
#define __ASM_MACH_EP93XX_SPI_H

struct spi_device;

/**
 * struct ep93xx_spi_info - EP93xx specific SPI descriptor
 * @num_chipselect: number chip selects supported
 * @use_dma: use DMA for the transfers
 */
struct ep93xx_spi_info {
	int	num_chipselect;
	bool	use_dma;
};

#endif /* __ASM_MACH_EP93XX_SPI_H */
