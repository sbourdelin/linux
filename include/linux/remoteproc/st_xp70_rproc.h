/*
 * st_xp70_rproc.h
 *
 * Copyright (C) 2016 STMicroelectronics
 * Author: Peter Griffin <peter.griffin@linaro.org>
 * License terms:  GNU General Public License (GPL), version 2
 */
#ifndef _ST_XP70_H
#define _ST_XP70_H

#define XP70_MEM_MAX 2
#define XP70_MAX_CLK 4
#define NAME_SZ 10

enum {
	DMEM,
	IMEM,
};

/**
 * struct xp70_mem - xp70 internal memory structure
 * @cpu_addr: MPU virtual address of the memory region
 * @bus_addr: Bus address used to access the memory region
 * @dev_addr: Device address from Wakeup M3 view
 * @size: Size of the memory region
 */
struct xp70_mem {
	void __iomem *cpu_addr;
	phys_addr_t bus_addr;
	u32 dev_addr;
	size_t size;
	struct resource *io_res;
};

/**
 * struct st_xp70_rproc - XP70 slim core
 * @rproc: rproc handle
 * @pdev: pointer to platform device
 * @mem: xp70 memory information
 * @slimcore: xp70 slimcore regs
 * @peri: xp70 peripheral regs
 * @clks: xp70 clocks
 */
struct st_xp70_rproc {
	struct rproc *rproc;
	struct platform_device *pdev;
	struct xp70_mem mem[XP70_MEM_MAX];
	void __iomem *slimcore;
	void __iomem *peri;
	struct clk *clks[XP70_MAX_CLK];
};

struct rproc *xp70_rproc_alloc(struct platform_device *pdev, char *fw_name);
void xp70_rproc_put(struct st_xp70_rproc *xp70_rproc);

#endif
