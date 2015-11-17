/*
* Copyright (c) 2015 MediaTek Inc.
* Author: Andrew-CT Chen <andrew-ct.chen@mediatek.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/sched.h>
#include <linux/sizes.h>

#include "mtk_vpu_core.h"

/**
 * VPU (video processor unit) is a tiny processor controlling video hardware
 * related to video codec, scaling and color format converting.
 * VPU interfaces with other blocks by share memory and interrupt.
 **/
#define MTK_VPU_DRV_NAME	"mtk_vpu"

#define INIT_TIMEOUT_MS		2000U
#define IPI_TIMEOUT_MS		2000U
#define VPU_FW_VER_LEN		16

/* vpu extended virtural address */
#define VPU_PMEM0_VIRT(vpu)	((vpu)->mem.p_va)
#define VPU_DMEM0_VIRT(vpu)	((vpu)->mem.d_va)
/* vpu extended iova address*/
#define VPU_PMEM0_IOVA(vpu)	((vpu)->mem.p_iova)
#define VPU_DMEM0_IOVA(vpu)	((vpu)->mem.d_iova)

#define VPU_PTCM(dev)		((dev)->reg.sram)
#define VPU_DTCM(dev)		((dev)->reg.sram + VPU_DTCM_OFFSET)

#define VPU_PTCM_SIZE		(96 * SZ_1K)
#define VPU_DTCM_SIZE		(32 * SZ_1K)
#define VPU_DTCM_OFFSET		0x18000UL
#define VPU_EXT_P_SIZE		SZ_1M
#define VPU_EXT_D_SIZE		SZ_4M
#define VPU_P_FW_SIZE		(VPU_PTCM_SIZE + VPU_EXT_P_SIZE)
#define VPU_D_FW_SIZE		(VPU_DTCM_SIZE + VPU_EXT_D_SIZE)
#define SHARE_BUF_SIZE		48

#define VPU_P_FW		"vpu_p.bin"
#define VPU_D_FW		"vpu_d.bin"

#define VPU_BASE		0x0
#define VPU_TCM_CFG		0x0008
#define VPU_PMEM_EXT0_ADDR	0x000C
#define VPU_PMEM_EXT1_ADDR	0x0010
#define VPU_TO_HOST		0x001C
#define VPU_DMEM_EXT0_ADDR	0x0014
#define VPU_DMEM_EXT1_ADDR	0x0018
#define HOST_TO_VPU		0x0024
#define VPU_PC_REG		0x0060
#define VPU_WDT_REG		0x0084

/* vpu inter-processor communication interrupt */
#define VPU_IPC_INT		BIT(8)

/**
 * enum vpu_fw_type - VPU firmware type
 *
 * @P_FW: program firmware
 * @D_FW: data firmware
 *
 */
enum vpu_fw_type {
	P_FW,
	D_FW,
};

/**
 * struct vpu_mem - VPU memory information
 *
 * @p_va:	the kernel virtual memory address of
 *		VPU extended program memory
 * @d_va:	the kernel virtual memory address of VPU extended data memory
 * @p_iova:	the iova memory address of VPU extended program memory
 * @d_iova:	the iova memory address of VPU extended data memory
 */
struct vpu_mem {
	void *p_va;
	void *d_va;
	dma_addr_t p_iova;
	dma_addr_t d_iova;
};

/**
 * struct vpu_regs - VPU SRAM and configuration registers
 *
 * @sram:	the register for VPU sram
 * @cfg:	the register for VPU configuration
 * @irq:	the irq number for VPU interrupt
 */
struct vpu_regs {
	void __iomem *sram;
	void __iomem *cfg;
	int irq;
};

/**
 * struct vpu_run - VPU initialization status
 *
 * @signaled:	the signal of vpu initialization completed
 * @fw_ver:	VPU firmware version
 * @wq:		wait queue for VPU initialization status
 */
struct vpu_run {
	u32 signaled;
	char fw_ver[VPU_FW_VER_LEN];
	wait_queue_head_t wq;
};

/**
 * struct vpu_ipi_desc - VPU IPI descriptor
 *
 * @handler:	IPI handler
 * @name:	the name of IPI handler
 * @priv:	the private data of IPI handler
 */
struct vpu_ipi_desc {
	ipi_handler_t handler;
	const char *name;
	void *priv;
};

/**
 * struct share_obj - The DTCM (Data Tightly-Coupled Memory) buffer shared with
 *		      AP and VPU
 *
 * @id:		IPI id
 * @len:	share buffer length
 * @share_buf:	share buffer data
 */
struct share_obj {
	int32_t id;
	uint32_t len;
	unsigned char share_buf[SHARE_BUF_SIZE];
};

/**
 * struct mtk_vpu - vpu driver data
 * @mem:		VPU extended memory information
 * @reg:		VPU SRAM and configuration registers
 * @run:		VPU initialization status
 * @ipi_desc:		VPU IPI descriptor
 * @recv_buf:		VPU DTCM share buffer for receiving. The
 *			receive buffer is only accessed in interrupt context.
 * @send_buf:		VPU DTCM share buffer for sending
 * @dev:		VPU struct device
 * @clk:		VPU clock on/off
 * @vpu_mutex:		protect mtk_vpu (except recv_buf) and ensure only
 *			one client to use VPU service at a time. For example,
 *			suppose a client is using VPU to decode VP8.
 *			If the other client wants to encode VP8,
 *			it has to wait until VP8 decode completes.
 *
 */
struct mtk_vpu {
	struct vpu_mem mem;
	struct vpu_regs reg;
	struct vpu_run run;
	struct vpu_ipi_desc ipi_desc[IPI_MAX];
	struct share_obj *recv_buf;
	struct share_obj *send_buf;
	struct device *dev;
	struct clk *clk;
	struct mutex vpu_mutex; /* for protecting vpu data data structure */
};

/* the thread calls the function should hold the |vpu_mutex| */
static inline void vpu_cfg_writel(struct mtk_vpu *vpu, u32 val, u32 offset)
{
	writel(val, vpu->reg.cfg + offset);
}

static inline u32 vpu_cfg_readl(struct mtk_vpu *vpu, u32 offset)
{
	return readl(vpu->reg.cfg + offset);
}

static inline bool vpu_running(struct mtk_vpu *vpu)
{
	return vpu_cfg_readl(vpu, VPU_BASE) & BIT(0);
}

void vpu_disable_clock(struct platform_device *pdev)
{
	struct mtk_vpu *vpu = platform_get_drvdata(pdev);

	/* Disable VPU watchdog */
	vpu_cfg_writel(vpu,
		       vpu_cfg_readl(vpu, VPU_WDT_REG) & ~(1L<<31),
		       VPU_WDT_REG);

	clk_disable_unprepare(vpu->clk);
}

int vpu_enable_clock(struct platform_device *pdev)
{
	struct mtk_vpu *vpu = platform_get_drvdata(pdev);
	int ret;

	ret = clk_prepare_enable(vpu->clk);
	if (ret)
		return ret;
	/* Enable VPU watchdog */
	vpu_cfg_writel(vpu, vpu_cfg_readl(vpu, VPU_WDT_REG) | (1L << 31),
		       VPU_WDT_REG);

	return ret;
}

int vpu_ipi_register(struct platform_device *pdev,
		     enum ipi_id id, ipi_handler_t handler,
		     const char *name, void *priv)
{
	struct mtk_vpu *vpu = platform_get_drvdata(pdev);
	struct vpu_ipi_desc *ipi_desc;

	if (!vpu) {
		dev_err(&pdev->dev, "vpu device in not ready\n");
		return -EPROBE_DEFER;
	}

	if (id < IPI_MAX && handler != NULL) {
		ipi_desc = vpu->ipi_desc;
		ipi_desc[id].name = name;
		ipi_desc[id].handler = handler;
		ipi_desc[id].priv = priv;
		return 0;
	}

	dev_err(&pdev->dev, "register vpu ipi with invalid arguments\n");
	return -EINVAL;
}

int vpu_ipi_send(struct platform_device *pdev,
		 enum ipi_id id, void *buf,
		 unsigned int len, unsigned int wait)
{
	struct mtk_vpu *vpu = platform_get_drvdata(pdev);
	struct share_obj *send_obj = vpu->send_buf;
	unsigned long timeout;

	if (id >= IPI_MAX || len > sizeof(send_obj->share_buf) || buf == NULL) {
		dev_err(vpu->dev, "failed to send ipi message\n");
		return -EINVAL;
	}

	if (!vpu_running(vpu)) {
		dev_err(vpu->dev, "vpu_ipi_send: VPU is not running\n");
		return -ENXIO;
	}

	mutex_lock(&vpu->vpu_mutex);
	if (vpu_cfg_readl(vpu, HOST_TO_VPU) && !wait) {
		mutex_unlock(&vpu->vpu_mutex);
		return -EBUSY;
	}

	if (wait)
		while (vpu_cfg_readl(vpu, HOST_TO_VPU))
			;

	memcpy((void *)send_obj->share_buf, buf, len);
	send_obj->len = len;
	send_obj->id = id;
	vpu_cfg_writel(vpu, 0x1, HOST_TO_VPU);

	/* Wait until VPU receives the command */
	timeout = jiffies + msecs_to_jiffies(IPI_TIMEOUT_MS);
	do {
		if (time_after(jiffies, timeout)) {
			dev_err(vpu->dev, "vpu_ipi_send: IPI timeout!\n");
			return -EIO;
		}
	} while (vpu_cfg_readl(vpu, HOST_TO_VPU));

	mutex_unlock(&vpu->vpu_mutex);

	return 0;
}

void *vpu_mapping_dm_addr(struct platform_device *pdev,
			  void *dtcm_dmem_addr)
{
	struct mtk_vpu *vpu = platform_get_drvdata(pdev);
	unsigned long p_vpu_dtcm = (unsigned long)VPU_DTCM(vpu);
	unsigned long ul_dtcm_dmem_addr = (unsigned long)(dtcm_dmem_addr);

	if (dtcm_dmem_addr == NULL ||
	    (ul_dtcm_dmem_addr > (VPU_DTCM_SIZE + VPU_EXT_D_SIZE))) {
		dev_err(vpu->dev, "invalid virtual data memory address\n");
		return ERR_PTR(-EINVAL);
	}

	if (ul_dtcm_dmem_addr < VPU_DTCM_SIZE)
		return (void *)(ul_dtcm_dmem_addr + p_vpu_dtcm);

	return (void *)((ul_dtcm_dmem_addr - VPU_DTCM_SIZE) +
		VPU_DMEM0_VIRT(vpu));
}

dma_addr_t *vpu_mapping_iommu_dm_addr(struct platform_device *pdev,
				      void *dmem_addr)
{
	unsigned long ul_dmem_addr = (unsigned long)(dmem_addr);
	struct mtk_vpu *vpu = platform_get_drvdata(pdev);

	if (dmem_addr == NULL ||
	    (ul_dmem_addr < VPU_DTCM_SIZE) ||
	    (ul_dmem_addr > (VPU_DTCM_SIZE + VPU_EXT_D_SIZE))) {
		dev_err(vpu->dev, "invalid IOMMU data memory address\n");
		return ERR_PTR(-EINVAL);
	}

	return (dma_addr_t *)((ul_dmem_addr - VPU_DTCM_SIZE) +
			       VPU_DMEM0_IOVA(vpu));
}

struct platform_device *vpu_get_plat_device(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *vpu_node;
	struct platform_device *vpu_pdev;

	vpu_node = of_parse_phandle(dev->of_node, "vpu", 0);
	if (!vpu_node) {
		dev_err(dev, "can't get vpu node\n");
		return NULL;
	}

	vpu_pdev = of_find_device_by_node(vpu_node);
	if (WARN_ON(!vpu_pdev)) {
		dev_err(dev, "vpu pdev failed\n");
		of_node_put(vpu_node);
		return NULL;
	}

	return vpu_pdev;
}

/* load vpu program/data memory */
static void load_requested_vpu(struct mtk_vpu *vpu,
			       size_t fw_size,
			       const u8 *fw_data,
			       u8 fw_type)
{
	size_t target_size = fw_type ? VPU_DTCM_SIZE : VPU_PTCM_SIZE;
	size_t extra_fw_size = 0;
	void *dest;

	/* reset VPU */
	vpu_cfg_writel(vpu, 0x0, VPU_BASE);

	/* handle extended firmware size */
	if (fw_size > target_size) {
		dev_dbg(vpu->dev, "fw size %lx > limited fw size %lx\n",
			fw_size, target_size);
		extra_fw_size = fw_size - target_size;
		dev_dbg(vpu->dev, "extra_fw_size %lx\n", extra_fw_size);
		fw_size = target_size;
	}
	dest = fw_type ? VPU_DTCM(vpu) : VPU_PTCM(vpu);
	memcpy(dest, fw_data, fw_size);
	/* download to extended memory if need */
	if (extra_fw_size > 0) {
		dest = fw_type ?
			VPU_DMEM0_VIRT(vpu) : VPU_PMEM0_VIRT(vpu);

		dev_dbg(vpu->dev, "download extended memory type %x\n",
			fw_type);
		memcpy(dest, fw_data + target_size, extra_fw_size);
	}
}

int vpu_load_firmware(struct platform_device *pdev)
{
	struct mtk_vpu *vpu = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	struct vpu_run *run = &vpu->run;
	const struct firmware *vpu_fw;
	int ret;

	if (!pdev) {
		dev_err(dev, "VPU platform device is invalid\n");
		return -EINVAL;
	}

	mutex_lock(&vpu->vpu_mutex);

	ret = vpu_enable_clock(pdev);
	if (ret) {
		dev_err(dev, "enable clock failed %d\n", ret);
		goto OUT_LOAD_FW;
	}

	if (vpu_running(vpu)) {
		vpu_disable_clock(pdev);
		mutex_unlock(&vpu->vpu_mutex);
		dev_warn(dev, "vpu is running already\n");
		return 0;
	}

	run->signaled = false;
	dev_dbg(vpu->dev, "firmware request\n");
	ret = request_firmware(&vpu_fw, VPU_P_FW, dev);
	if (ret < 0) {
		dev_err(dev, "Failed to load %s, %d\n", VPU_P_FW, ret);
		goto OUT_LOAD_FW;
	}
	if (vpu_fw->size > VPU_P_FW_SIZE) {
		ret = -EFBIG;
		dev_err(dev, "program fw size %zu is abnormal\n", vpu_fw->size);
		goto OUT_LOAD_FW;
	}
	dev_dbg(vpu->dev, "Downloaded program fw size: %zu.\n",
		vpu_fw->size);
	/* Downloading program firmware to device*/
	load_requested_vpu(vpu, vpu_fw->size, vpu_fw->data,
			   P_FW);
	release_firmware(vpu_fw);

	ret = request_firmware(&vpu_fw, VPU_D_FW, dev);
	if (ret < 0) {
		dev_err(dev, "Failed to load %s, %d\n", VPU_D_FW, ret);
		goto OUT_LOAD_FW;
	}
	if (vpu_fw->size > VPU_D_FW_SIZE) {
		ret = -EFBIG;
		dev_err(dev, "data fw size %zu is abnormal\n", vpu_fw->size);
		goto OUT_LOAD_FW;
	}
	dev_dbg(vpu->dev, "Downloaded data fw size: %zu.\n",
		vpu_fw->size);
	/* Downloading data firmware to device */
	load_requested_vpu(vpu, vpu_fw->size, vpu_fw->data,
			   D_FW);
	release_firmware(vpu_fw);
	/* boot up vpu */
	vpu_cfg_writel(vpu, 0x1, VPU_BASE);

	ret = wait_event_interruptible_timeout(run->wq,
					       run->signaled,
					       msecs_to_jiffies(INIT_TIMEOUT_MS)
					       );
	if (0 == ret) {
		ret = -ETIME;
		dev_err(dev, "wait vpu initialization timout!\n");
		goto OUT_LOAD_FW;
	} else if (-ERESTARTSYS == ret) {
		dev_err(dev, "wait vpu interrupted by a signal!\n");
		goto OUT_LOAD_FW;
	}

	ret = 0;
	dev_info(dev, "vpu is ready. Fw version %s\n", run->fw_ver);

OUT_LOAD_FW:
	vpu_disable_clock(pdev);
	mutex_unlock(&vpu->vpu_mutex);

	return ret;
}

static void vpu_init_ipi_handler(void *data, unsigned int len, void *priv)
{
	struct mtk_vpu *vpu = (struct mtk_vpu *)priv;
	struct vpu_run *run = (struct vpu_run *)data;

	vpu->run.signaled = run->signaled;
	strncpy(vpu->run.fw_ver, run->fw_ver, VPU_FW_VER_LEN);
	wake_up_interruptible(&vpu->run.wq);
}

static int vpu_debug_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t vpu_debug_read(struct file *file, char __user *user_buf,
			      size_t count, loff_t *ppos)
{
	char buf[256];
	unsigned int len;
	unsigned int running, pc, vpu_to_host, host_to_vpu, wdt;
	int ret;
	struct device *dev = file->private_data;
	struct platform_device *pdev = to_platform_device(dev);
	struct mtk_vpu *vpu = dev_get_drvdata(dev);

	ret = vpu_enable_clock(pdev);
	if (ret) {
		dev_err(vpu->dev, "[VPU] enable clock failed %d\n", ret);
		return 0;
	}

	/* vpu register status */
	running = vpu_running(vpu);
	pc = vpu_cfg_readl(vpu, VPU_PC_REG);
	wdt = vpu_cfg_readl(vpu, VPU_WDT_REG);
	host_to_vpu = vpu_cfg_readl(vpu, HOST_TO_VPU);
	vpu_to_host = vpu_cfg_readl(vpu, VPU_TO_HOST);
	vpu_disable_clock(pdev);

	if (running) {
		len = sprintf(buf, "VPU is running\n\n"
		"FW Version: %s\n"
		"PC: 0x%x\n"
		"WDT: 0x%x\n"
		"Host to VPU: 0x%x\n"
		"VPU to Host: 0x%x\n",
		vpu->run.fw_ver, pc, wdt,
		host_to_vpu, vpu_to_host);
	} else {
		len = sprintf(buf, "VPU not running\n");
	}

	return simple_read_from_buffer(user_buf, count, ppos, buf, len);
}

static const struct file_operations vpu_debug_fops = {
	.open = vpu_debug_open,
	.read = vpu_debug_read,
};

static void vpu_free_p_ext_mem(struct mtk_vpu *vpu)
{
	struct device *dev = vpu->dev;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);

	dma_free_coherent(dev, VPU_EXT_P_SIZE, VPU_PMEM0_VIRT(vpu),
			  VPU_PMEM0_IOVA(vpu));

	if (domain)
		iommu_detach_device(domain, vpu->dev);
}

static void vpu_free_d_ext_mem(struct mtk_vpu *vpu)
{
	struct device *dev = vpu->dev;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);

	dma_free_coherent(dev, VPU_EXT_D_SIZE, VPU_DMEM0_VIRT(vpu),
			  VPU_DMEM0_IOVA(vpu));

	if (domain)
		iommu_detach_device(domain, dev);
}

static int vpu_alloc_p_ext_mem(struct mtk_vpu *vpu)
{
	struct device *dev = vpu->dev;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	phys_addr_t p_pa;

	VPU_PMEM0_VIRT(vpu) = dma_alloc_coherent(dev,
						 VPU_EXT_P_SIZE,
						 &(VPU_PMEM0_IOVA(vpu)),
						 GFP_KERNEL);
	if (VPU_PMEM0_VIRT(vpu) == NULL) {
		dev_err(dev, "Failed to allocate the extended program memory\n");
		return PTR_ERR(VPU_PMEM0_VIRT(vpu));
	}

	p_pa = iommu_iova_to_phys(domain, vpu->mem.p_iova);
	/* Disable extend0. Enable extend1 */
	vpu_cfg_writel(vpu, 0x1, VPU_PMEM_EXT0_ADDR);
	vpu_cfg_writel(vpu, (p_pa & 0xFFFFF000), VPU_PMEM_EXT1_ADDR);

	dev_info(dev, "Program extend memory phy=0x%llx virt=0x%p iova=0x%llx\n",
		 (unsigned long long)p_pa,
		 VPU_PMEM0_VIRT(vpu),
		 (unsigned long long)VPU_PMEM0_IOVA(vpu));

	return 0;
}

static int vpu_alloc_d_ext_mem(struct mtk_vpu *vpu)
{
	struct device *dev = vpu->dev;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	phys_addr_t d_pa;

	VPU_DMEM0_VIRT(vpu) = dma_alloc_coherent(dev,
						 VPU_EXT_D_SIZE,
						 &(VPU_DMEM0_IOVA(vpu)),
						 GFP_KERNEL);
	if (VPU_DMEM0_VIRT(vpu) == NULL) {
		dev_err(dev, "Failed to allocate the extended data memory\n");
		return PTR_ERR(VPU_DMEM0_VIRT(vpu));
	}

	d_pa = iommu_iova_to_phys(domain, vpu->mem.d_iova);

	/* Disable extend0. Enable extend1 */
	vpu_cfg_writel(vpu, 0x1, VPU_DMEM_EXT0_ADDR);
	vpu_cfg_writel(vpu, (d_pa & 0xFFFFF000),
		       VPU_DMEM_EXT1_ADDR);

	dev_info(dev, "Data extend memory phy=0x%llx virt=0x%p iova=0x%llx\n",
		 (unsigned long long)d_pa,
		 VPU_DMEM0_VIRT(vpu),
		 (unsigned long long)VPU_DMEM0_IOVA(vpu));

	return 0;
}

static void vpu_ipi_handler(struct mtk_vpu *vpu)
{
	struct share_obj *rcv_obj = vpu->recv_buf;
	struct vpu_ipi_desc *ipi_desc = vpu->ipi_desc;

	if (rcv_obj->id < IPI_MAX && ipi_desc[rcv_obj->id].handler) {
		ipi_desc[rcv_obj->id].handler(rcv_obj->share_buf,
					      rcv_obj->len,
					      ipi_desc[rcv_obj->id].priv);
	} else {
		dev_err(vpu->dev, "No such ipi id = %d\n", rcv_obj->id);
	}
}

static int vpu_ipi_init(struct mtk_vpu *vpu)
{
	/* Disable VPU to host interrupt */
	vpu_cfg_writel(vpu, 0x0, VPU_TO_HOST);

	/* shared buffer initialization */
	vpu->recv_buf = (struct share_obj *)VPU_DTCM(vpu);
	vpu->send_buf = vpu->recv_buf + 1;
	memset(vpu->recv_buf, 0, sizeof(struct share_obj));
	memset(vpu->send_buf, 0, sizeof(struct share_obj));
	mutex_init(&vpu->vpu_mutex);

	return 0;
}

static irqreturn_t vpu_irq_handler(int irq, void *priv)
{
	struct mtk_vpu *vpu = priv;
	uint32_t vpu_to_host = vpu_cfg_readl(vpu, VPU_TO_HOST);

	if (vpu_to_host & VPU_IPC_INT)
		vpu_ipi_handler(vpu);
	else
		dev_err(vpu->dev, "vpu watchdog timeout!\n");

	/* VPU won't send another interrupt until we set VPU_TO_HOST to 0. */
	vpu_cfg_writel(vpu, 0x0, VPU_TO_HOST);

	return IRQ_HANDLED;
}

static struct dentry *vpu_debugfs;
static int mtk_vpu_probe(struct platform_device *pdev)
{
	struct mtk_vpu *vpu;
	struct device *dev;
	struct resource *res;
	int ret = 0;

	dev_dbg(&pdev->dev, "initialization\n");

	dev = &pdev->dev;
	vpu = devm_kzalloc(dev, sizeof(*vpu), GFP_KERNEL);
	if (!vpu)
		return -ENOMEM;

	vpu->dev = &pdev->dev;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram");
	vpu->reg.sram = devm_ioremap_resource(dev, res);
	if (IS_ERR(vpu->reg.sram)) {
		dev_err(dev, "devm_ioremap_resource vpu sram failed.\n");
		return PTR_ERR(vpu->reg.sram);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg_reg");
	vpu->reg.cfg = devm_ioremap_resource(dev, res);
	if (IS_ERR(vpu->reg.cfg)) {
		dev_err(dev, "devm_ioremap_resource vpu cfg failed.\n");
		return PTR_ERR(vpu->reg.cfg);
	}

	/* Get VPU clock */
	vpu->clk = devm_clk_get(dev, "main");
	if (vpu->clk == NULL) {
		dev_err(dev, "get vpu clock fail\n");
		return -EINVAL;
	}

	platform_set_drvdata(pdev, vpu);

	ret = vpu_enable_clock(pdev);
	if (ret) {
		ret = -EINVAL;
		return ret;
	}

	dev_dbg(dev, "vpu ipi init\n");
	ret = vpu_ipi_init(vpu);
	if (ret) {
		dev_err(dev, "Failed to init ipi\n");
		goto disable_vpu_clk;
	}

	platform_set_drvdata(pdev, vpu);

	/* register vpu initialization IPI */
	ret = vpu_ipi_register(pdev, IPI_VPU_INIT, vpu_init_ipi_handler,
			       "vpu_init", vpu);
	if (ret) {
		dev_err(dev, "Failed to register IPI_VPU_INIT\n");
		goto vpu_mutex_destroy;
	}

	vpu_debugfs = debugfs_create_file("mtk_vpu", S_IRUGO, NULL, (void *)dev,
					  &vpu_debug_fops);
	if (!vpu_debugfs) {
		ret = -ENOMEM;
		goto cleanup_ipi;
	}

	/* Set PTCM to 96K and DTCM to 32K */
	vpu_cfg_writel(vpu, 0x2, VPU_TCM_CFG);

	ret = vpu_alloc_p_ext_mem(vpu);
	if (ret) {
		dev_err(dev, "Allocate PM failed\n");
		goto remove_debugfs;
	}

	ret = vpu_alloc_d_ext_mem(vpu);
	if (ret) {
		dev_err(dev, "Allocate DM failed\n");
		goto free_p_mem;
	}

	init_waitqueue_head(&vpu->run.wq);

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (res == NULL) {
		dev_err(dev, "get IRQ resource failed.\n");
		ret = -ENXIO;
		goto free_d_mem;
	}
	vpu->reg.irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(dev, vpu->reg.irq, vpu_irq_handler, 0,
			       pdev->name, vpu);
	if (ret) {
		dev_err(dev, "failed to request irq\n");
		goto free_d_mem;
	}

	vpu_disable_clock(pdev);
	dev_dbg(dev, "initialization completed\n");

	return 0;

free_d_mem:
	vpu_free_d_ext_mem(vpu);
free_p_mem:
	vpu_free_p_ext_mem(vpu);
remove_debugfs:
	debugfs_remove(vpu_debugfs);
cleanup_ipi:
	memset(vpu->ipi_desc, 0, sizeof(struct vpu_ipi_desc)*IPI_MAX);
vpu_mutex_destroy:
	mutex_destroy(&vpu->vpu_mutex);
disable_vpu_clk:
	vpu_disable_clock(pdev);

	return ret;
}

static const struct of_device_id mtk_vpu_match[] = {
	{
		.compatible = "mediatek,mt8173-vpu",
	},
	{},
};
MODULE_DEVICE_TABLE(of, mtk_vpu_match);

static int mtk_vpu_remove(struct platform_device *pdev)
{
	struct mtk_vpu *vpu = platform_get_drvdata(pdev);

	vpu_free_p_ext_mem(vpu);
	vpu_free_d_ext_mem(vpu);

	return 0;
}

static struct platform_driver mtk_vpu_driver = {
	.probe	= mtk_vpu_probe,
	.remove	= mtk_vpu_remove,
	.driver	= {
		.name	= MTK_VPU_DRV_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = mtk_vpu_match,
	},
};

module_platform_driver(mtk_vpu_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Mediatek Video Prosessor Unit driver");
