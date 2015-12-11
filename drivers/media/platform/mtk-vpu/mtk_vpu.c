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

#include "mtk_vpu.h"

/**
 * VPU (video processor unit) is a tiny processor controlling video hardware
 * related to video codec, scaling and color format converting.
 * VPU interfaces with other blocks by share memory and interrupt.
 **/

#define INIT_TIMEOUT_MS		2000U
#define IPI_TIMEOUT_MS		2000U
#define VPU_FW_VER_LEN		16

/* maximum program/data TCM (Tightly-Coupled Memory) size */
#define VPU_PTCM_SIZE		(96 * SZ_1K)
#define VPU_DTCM_SIZE		(32 * SZ_1K)
/* the offset to get data tcm address */
#define VPU_DTCM_OFFSET		0x18000UL
/* daynamic allocated maximum extended memory size */
#define VPU_EXT_P_SIZE		SZ_1M
#define VPU_EXT_D_SIZE		SZ_4M
/* maximum binary firmware size */
#define VPU_P_FW_SIZE		(VPU_PTCM_SIZE + VPU_EXT_P_SIZE)
#define VPU_D_FW_SIZE		(VPU_DTCM_SIZE + VPU_EXT_D_SIZE)
/* the size of share buffer between Host and  VPU */
#define SHARE_BUF_SIZE		48

/* binary firmware name */
#define VPU_P_FW		"vpu_p.bin"
#define VPU_D_FW		"vpu_d.bin"

#define VPU_RESET		0x0
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
 * struct vpu_mem - VPU extended program/data memory information
 *
 * @va:		the kernel virtual memory address of VPU extended memory
 * @iova:	the iova memory address of VPU extended memory
 *
 */
struct vpu_mem {
	void *va;
	dma_addr_t iova;
};

/**
 * struct vpu_regs - VPU TCM and configuration registers
 *
 * @tcm:	the register for VPU Tightly-Coupled Memory
 * @cfg:	the register for VPU configuration
 * @irq:	the irq number for VPU interrupt
 */
struct vpu_regs {
	void __iomem *tcm;
	void __iomem *cfg;
	int irq;
};

/**
 * struct vpu_run - VPU initialization status
 *
 * @signaled:		the signal of vpu initialization completed
 * @fw_ver:		VPU firmware version
 * @enc_capability:	encoder capability
 * @wq:			wait queue for VPU initialization status
 */
struct vpu_run {
	u32 signaled;
	char fw_ver[VPU_FW_VER_LEN];
	unsigned int	enc_capability;
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
 * struct share_obj - DTCM (Data Tightly-Coupled Memory) buffer shared with
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
 * @extmem:		VPU extended memory information
 * @reg:		VPU TCM and configuration registers
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
 * @wdt_refcnt		WDT reference count to make sure the watchdog can be
 *			disabled if no other client is using VPU service
 * @ipi_ack_signaled:	The ACKs for registered IPI function sending
 *			interrupt to VPU
 * @ack_wq:		The wait queue for each codec and mdp. When sleeping
 *			processes wake up, they will check the condition
 *			"ipi_ack_signaled" to run the corresponding action or
 *			go back to sleep.
 *
 */
struct mtk_vpu {
	struct vpu_mem extmem[2];
	struct vpu_regs reg;
	struct vpu_run run;
	struct vpu_ipi_desc ipi_desc[IPI_MAX];
	struct share_obj *recv_buf;
	struct share_obj *send_buf;
	struct device *dev;
	struct clk *clk;
	struct mutex vpu_mutex; /* for protecting vpu data data structure */
	atomic_t wdt_refcnt;
	wait_queue_head_t ack_wq;
	bool ipi_id_ack[IPI_MAX];
};

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
	return vpu_cfg_readl(vpu, VPU_RESET) & BIT(0);
}

void vpu_clock_disable(struct mtk_vpu *vpu)
{
	/* Disable VPU watchdog */
	if (atomic_dec_and_test(&vpu->wdt_refcnt))
		vpu_cfg_writel(vpu,
			       vpu_cfg_readl(vpu, VPU_WDT_REG) & ~(1L<<31),
			       VPU_WDT_REG);

	clk_disable(vpu->clk);
}

int vpu_clock_enable(struct mtk_vpu *vpu)
{
	int ret;

	ret = clk_enable(vpu->clk);
	if (ret)
		return ret;
	/* Enable VPU watchdog */
	if (!atomic_read(&vpu->wdt_refcnt))
		vpu_cfg_writel(vpu,
			       vpu_cfg_readl(vpu, VPU_WDT_REG) | (1L << 31),
			       VPU_WDT_REG);

	atomic_inc(&vpu->wdt_refcnt);

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
		 unsigned int len)
{
	struct mtk_vpu *vpu = platform_get_drvdata(pdev);
	struct share_obj *send_obj = vpu->send_buf;
	unsigned long timeout;
	int ret = 0;

	if (id <= IPI_VPU_INIT || id >= IPI_MAX ||
	    len > sizeof(send_obj->share_buf) || buf == NULL) {
		dev_err(vpu->dev, "failed to send ipi message\n");
		return -EINVAL;
	}

	ret = vpu_clock_enable(vpu);
	if (ret) {
		dev_err(vpu->dev, "failed to enable vpu clock\n");
		return ret;
	}
	if (!vpu_running(vpu)) {
		dev_err(vpu->dev, "vpu_ipi_send: VPU is not running\n");
		goto clock_disable;
	}

	mutex_lock(&vpu->vpu_mutex);

	 /* Wait until VPU receives the last command */
	timeout = jiffies + msecs_to_jiffies(IPI_TIMEOUT_MS);
	do {
		if (time_after(jiffies, timeout)) {
			dev_err(vpu->dev, "vpu_ipi_send: IPI timeout!\n");
			ret = -EIO;
			goto mut_unlock;
		}
	} while (vpu_cfg_readl(vpu, HOST_TO_VPU));

	memcpy((void *)send_obj->share_buf, buf, len);
	send_obj->len = len;
	send_obj->id = id;

	vpu->ipi_id_ack[id] = false;
	/* send the command to VPU */
	vpu_cfg_writel(vpu, 0x1, HOST_TO_VPU);

	mutex_unlock(&vpu->vpu_mutex);

	/* wait for VPU's ACK */
	timeout = msecs_to_jiffies(IPI_TIMEOUT_MS);
	ret = wait_event_interruptible_timeout(vpu->ack_wq,
					       vpu->ipi_id_ack[id], timeout);
	vpu->ipi_id_ack[id] = false;
	if (0 == ret) {
		dev_err(vpu->dev, "vpu ipi %d ack time out !", id);
		ret = -EIO;
		goto clock_disable;
	} else if (-ERESTARTSYS == ret) {
		dev_err(vpu->dev, "vpu ipi %d ack wait interrupted by a signal",
			id);
		ret = -ERESTARTSYS;
		goto clock_disable;
	}
	vpu_clock_disable(vpu);

	return 0;

mut_unlock:
	vpu->ipi_id_ack[id] = false;
	mutex_unlock(&vpu->vpu_mutex);
clock_disable:
	vpu_clock_disable(vpu);

	return ret;
}

unsigned int vpu_get_venc_hw_capa(struct platform_device *pdev)
{
	struct mtk_vpu *vpu = platform_get_drvdata(pdev);

	return vpu->run.enc_capability;
}

void *vpu_mapping_dm_addr(struct platform_device *pdev,
			  u32 dtcm_dmem_addr)
{
	struct mtk_vpu *vpu = platform_get_drvdata(pdev);

	if (!dtcm_dmem_addr ||
	    (dtcm_dmem_addr > (VPU_DTCM_SIZE + VPU_EXT_D_SIZE))) {
		dev_err(vpu->dev, "invalid virtual data memory address\n");
		return ERR_PTR(-EINVAL);
	}

	if (dtcm_dmem_addr < VPU_DTCM_SIZE)
		return dtcm_dmem_addr + vpu->reg.tcm + VPU_DTCM_OFFSET;

	return vpu->extmem[D_FW].va + (dtcm_dmem_addr - VPU_DTCM_SIZE);
}

dma_addr_t vpu_mapping_iommu_dm_addr(struct platform_device *pdev,
				     u32 dmem_addr)
{
	struct mtk_vpu *vpu = platform_get_drvdata(pdev);

	if (!dmem_addr ||
	    (dmem_addr < VPU_DTCM_SIZE) ||
	    (dmem_addr > (VPU_DTCM_SIZE + VPU_EXT_D_SIZE))) {
		dev_err(vpu->dev, "invalid IOMMU data memory address\n");
		return -EINVAL;
	}

	return vpu->extmem[D_FW].iova + (dmem_addr - VPU_DTCM_SIZE);
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
static int load_requested_vpu(struct mtk_vpu *vpu,
			      const struct firmware *vpu_fw,
			      u8 fw_type)
{
	size_t tcm_size = fw_type ? VPU_DTCM_SIZE : VPU_PTCM_SIZE;
	size_t fw_size = fw_type ? VPU_D_FW_SIZE : VPU_P_FW_SIZE;
	char *fw_name = fw_type ? VPU_D_FW : VPU_P_FW;
	size_t dl_size = 0;
	size_t extra_fw_size = 0;
	void *dest;
	int ret;

	ret = request_firmware(&vpu_fw, fw_name, vpu->dev);
	if (ret < 0) {
		dev_err(vpu->dev, "Failed to load %s, %d\n", fw_name, ret);
		return ret;
	}
	dl_size = vpu_fw->size;
	if (dl_size > fw_size) {
		dev_err(vpu->dev, "fw %s size %zu is abnormal\n", fw_name,
			dl_size);
		release_firmware(vpu_fw);
		return  -EFBIG;
	}
	dev_dbg(vpu->dev, "Downloaded fw %s size: %zu.\n",
		fw_name,
		dl_size);
	/* reset VPU */
	vpu_cfg_writel(vpu, 0x0, VPU_RESET);

	/* handle extended firmware size */
	if (dl_size > tcm_size) {
		dev_dbg(vpu->dev, "fw size %lx > limited fw size %lx\n",
			dl_size, tcm_size);
		extra_fw_size = dl_size - tcm_size;
		dev_dbg(vpu->dev, "extra_fw_size %lx\n", extra_fw_size);
		dl_size = tcm_size;
	}
	dest = vpu->reg.tcm;
	if (fw_type == D_FW)
		dest += VPU_DTCM_OFFSET;
	memcpy(dest, vpu_fw->data, dl_size);
	/* download to extended memory if need */
	if (extra_fw_size > 0) {
		dest = vpu->extmem[fw_type].va;
		dev_dbg(vpu->dev, "download extended memory type %x\n",
			fw_type);
		memcpy(dest, vpu_fw->data + tcm_size, extra_fw_size);
	}

	release_firmware(vpu_fw);

	return 0;
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

	ret = vpu_clock_enable(vpu);
	if (ret) {
		dev_err(dev, "enable clock failed %d\n", ret);
		goto OUT_LOAD_FW;
	}

	if (vpu_running(vpu)) {
		vpu_clock_disable(vpu);
		mutex_unlock(&vpu->vpu_mutex);
		dev_warn(dev, "vpu is running already\n");
		return 0;
	}

	run->signaled = false;
	dev_dbg(vpu->dev, "firmware request\n");
	/* Downloading program firmware to device*/
	ret = load_requested_vpu(vpu, vpu_fw, P_FW);
	if (ret < 0) {
		dev_err(dev, "Failed to request %s, %d\n", VPU_P_FW, ret);
		goto OUT_LOAD_FW;
	}

	/* Downloading data firmware to device */
	ret = load_requested_vpu(vpu, vpu_fw, D_FW);
	if (ret < 0) {
		dev_err(dev, "Failed to request %s, %d\n", VPU_D_FW, ret);
		goto OUT_LOAD_FW;
	}

	/* boot up vpu */
	vpu_cfg_writel(vpu, 0x1, VPU_RESET);

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
	vpu_clock_disable(vpu);
	mutex_unlock(&vpu->vpu_mutex);

	return ret;
}

static void vpu_init_ipi_handler(void *data, unsigned int len, void *priv)
{
	struct mtk_vpu *vpu = (struct mtk_vpu *)priv;
	struct vpu_run *run = (struct vpu_run *)data;

	vpu->run.signaled = run->signaled;
	strncpy(vpu->run.fw_ver, run->fw_ver, VPU_FW_VER_LEN);
	vpu->run.enc_capability = run->enc_capability;
	wake_up_interruptible(&vpu->run.wq);
}

#ifdef CONFIG_DEBUG_FS
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
	struct mtk_vpu *vpu = dev_get_drvdata(dev);

	ret = vpu_clock_enable(vpu);
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
	vpu_clock_disable(vpu);

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
#endif /* CONFIG_DEBUG_FS */

static void vpu_free_ext_mem(struct mtk_vpu *vpu, u8 fw_type)
{
	struct device *dev = vpu->dev;
	size_t fw_ext_size = fw_type ? VPU_EXT_D_SIZE : VPU_EXT_P_SIZE;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);

	dma_free_coherent(dev, fw_ext_size, vpu->extmem[fw_type].va,
			  vpu->extmem[fw_type].iova);

	if (domain)
		iommu_detach_device(domain, vpu->dev);
}

static int vpu_alloc_ext_mem(struct mtk_vpu *vpu, u32 fw_type)
{
	struct device *dev = vpu->dev;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	phys_addr_t pa;
	size_t fw_ext_size = fw_type ? VPU_EXT_D_SIZE : VPU_EXT_P_SIZE;
	u32 vpu_ext_mem0 = fw_type ? VPU_DMEM_EXT0_ADDR : VPU_PMEM_EXT0_ADDR;
	u32 vpu_ext_mem1 = fw_type ? VPU_DMEM_EXT1_ADDR : VPU_PMEM_EXT1_ADDR;

	vpu->extmem[fw_type].va = dma_alloc_coherent(dev,
					       fw_ext_size,
					       &vpu->extmem[fw_type].iova,
					       GFP_KERNEL);
	if (vpu->extmem[fw_type].va == NULL) {
		dev_err(dev, "Failed to allocate the extended program memory\n");
		return PTR_ERR(vpu->extmem[fw_type].va);
	}

	pa = iommu_iova_to_phys(domain, vpu->extmem[fw_type].iova);
	/* Disable extend0. Enable extend1 */
	vpu_cfg_writel(vpu, 0x1, vpu_ext_mem0);
	vpu_cfg_writel(vpu, (pa & 0xFFFFF000), vpu_ext_mem1);

	dev_info(dev, "Program extend memory phy=0x%llx virt=0x%p iova=0x%llx\n",
		 (unsigned long long)pa,
		 vpu->extmem[fw_type].va,
		 (unsigned long long)vpu->extmem[fw_type].iova);

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
		if (rcv_obj->id > IPI_VPU_INIT) {
			vpu->ipi_id_ack[rcv_obj->id] = true;
			wake_up_interruptible(&vpu->ack_wq);
		}
	} else {
		dev_err(vpu->dev, "No such ipi id = %d\n", rcv_obj->id);
	}
}

static int vpu_ipi_init(struct mtk_vpu *vpu)
{
	/* Disable VPU to host interrupt */
	vpu_cfg_writel(vpu, 0x0, VPU_TO_HOST);

	/* shared buffer initialization */
	vpu->recv_buf = (struct share_obj *)(vpu->reg.tcm + VPU_DTCM_OFFSET);
	vpu->send_buf = vpu->recv_buf + 1;
	memset(vpu->recv_buf, 0, sizeof(struct share_obj));
	memset(vpu->send_buf, 0, sizeof(struct share_obj));
	mutex_init(&vpu->vpu_mutex);

	return 0;
}

static irqreturn_t vpu_irq_handler(int irq, void *priv)
{
	struct mtk_vpu *vpu = priv;
	uint32_t vpu_to_host;

	/*
	 * Clock should have been enabled already.
	 * Enable again in case vpu_ipi_send times out
	 * and has disabled the clock.
	 */
	clk_enable(vpu->clk);
	vpu_to_host = vpu_cfg_readl(vpu, VPU_TO_HOST);
	if (vpu_to_host & VPU_IPC_INT)
		vpu_ipi_handler(vpu);
	else
	/*
	 * TO DO:
	 * Reset video hardware
	 * Reset and redownload vpu firmware
	 */
		dev_err(vpu->dev, "vpu watchdog timeout! 0x%x", vpu_to_host);

	/* VPU won't send another interrupt until we set VPU_TO_HOST to 0. */
	vpu_cfg_writel(vpu, 0x0, VPU_TO_HOST);
	clk_disable(vpu->clk);

	return IRQ_HANDLED;
}

#ifdef CONFIG_DEBUG_FS
static struct dentry *vpu_debugfs;
#endif
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
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "tcm");
	vpu->reg.tcm = devm_ioremap_resource(dev, res);
	if (IS_ERR(vpu->reg.tcm)) {
		dev_err(dev, "devm_ioremap_resource vpu tcm failed.\n");
		return PTR_ERR(vpu->reg.tcm);
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
		dev_err(dev, "get vpu clock failed\n");
		return -EINVAL;
	}

	platform_set_drvdata(pdev, vpu);

	ret = clk_prepare(vpu->clk);
	if (ret) {
		dev_err(dev, "prepare vpu clock failed\n");
		return ret;
	}
	ret = vpu_clock_enable(vpu);
	if (ret) {
		dev_err(dev, "enable vpu clock failed\n");
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

#ifdef CONFIG_DEBUG_FS
	vpu_debugfs = debugfs_create_file("mtk_vpu", S_IRUGO, NULL, (void *)dev,
					  &vpu_debug_fops);
	if (!vpu_debugfs) {
		ret = -ENOMEM;
		goto cleanup_ipi;
	}
#endif

	/* Set PTCM to 96K and DTCM to 32K */
	vpu_cfg_writel(vpu, 0x2, VPU_TCM_CFG);

	ret = vpu_alloc_ext_mem(vpu, P_FW);
	if (ret) {
		dev_err(dev, "Allocate PM failed\n");
		goto remove_debugfs;
	}

	ret = vpu_alloc_ext_mem(vpu, D_FW);
	if (ret) {
		dev_err(dev, "Allocate DM failed\n");
		goto free_p_mem;
	}

	init_waitqueue_head(&vpu->run.wq);
	init_waitqueue_head(&vpu->ack_wq);

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

	vpu_clock_disable(vpu);
	dev_dbg(dev, "initialization completed\n");

	return 0;

free_d_mem:
	vpu_free_ext_mem(vpu, D_FW);
free_p_mem:
	vpu_free_ext_mem(vpu, P_FW);
remove_debugfs:
#ifdef CONFIG_DEBUG_FS
	debugfs_remove(vpu_debugfs);
cleanup_ipi:
#endif
	memset(vpu->ipi_desc, 0, sizeof(struct vpu_ipi_desc)*IPI_MAX);
vpu_mutex_destroy:
	mutex_destroy(&vpu->vpu_mutex);
disable_vpu_clk:
	vpu_clock_disable(vpu);

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

#ifdef CONFIG_DEBUG_FS
	debugfs_remove(vpu_debugfs);
#endif
	vpu_free_ext_mem(vpu, P_FW);
	vpu_free_ext_mem(vpu, D_FW);
	mutex_destroy(&vpu->vpu_mutex);
	clk_unprepare(vpu->clk);

	return 0;
}

static struct platform_driver mtk_vpu_driver = {
	.probe	= mtk_vpu_probe,
	.remove	= mtk_vpu_remove,
	.driver	= {
		.name	= "mtk_vpu",
		.owner	= THIS_MODULE,
		.of_match_table = mtk_vpu_match,
	},
};

module_platform_driver(mtk_vpu_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Mediatek Video Prosessor Unit driver");
