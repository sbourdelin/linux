/*
 * st_slim_rproc.h
 *
 * Copyright (C) 2016 STMicroelectronics
 * Author: Peter Griffin <peter.griffin@linaro.org>
 * License terms:  GNU General Public License (GPL), version 2
 */
#ifndef _ST_SLIM_H
#define _ST_SLIM_H

#define SLIM_MEM_MAX 2
#define SLIM_MAX_CLK 4

enum {
	SLIM_DMEM,
	SLIM_IMEM,
};

/**
 * struct slim_mem - slim internal memory structure
 * @cpu_addr: MPU virtual address of the memory region
 * @bus_addr: Bus address used to access the memory region
 * @size: Size of the memory region
 */
struct slim_mem {
	void __iomem *cpu_addr;
	phys_addr_t bus_addr;
	size_t size;
};

/**
 * struct st_slim_rproc - SLIM slim core
 * @rproc: rproc handle
 * @mem: slim memory information
 * @slimcore: slim slimcore regs
 * @peri: slim peripheral regs
 * @clks: slim clocks
 */
struct st_slim_rproc {
	struct rproc *rproc;
	struct slim_mem mem[SLIM_MEM_MAX];
	void __iomem *slimcore;
	void __iomem *peri;

	/* st_slim_rproc private */
	struct clk *clks[SLIM_MAX_CLK];
};

struct st_slim_rproc *slim_rproc_alloc(struct platform_device *pdev,
					char *fw_name);
void slim_rproc_put(struct st_slim_rproc *slim_rproc);

#endif
