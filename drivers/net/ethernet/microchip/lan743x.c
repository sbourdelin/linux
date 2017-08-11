/*
 * Copyright (C) 2017 Microchip Technology
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/crc32.h>
#include <linux/microchipphy.h>
#include <linux/net_tstamp.h>
#include <linux/phy.h>
#include "lan743x.h"

#define DRIVER_AUTHOR   "Bryan Whitehead <Bryan.Whitehead@microchip.com>"
#define DRIVER_DESC "LAN743x PCIe Gigabit Ethernet Driver"
#define DRIVER_NAME "lan743x"
#define DRIVER_VERSION  "0.2.0.0"

/* use ethtool to change the message enable for any given adapter */
static int msg_enable =	NETIF_MSG_DRV | NETIF_MSG_PROBE | NETIF_MSG_LINK |
			NETIF_MSG_IFUP | NETIF_MSG_IFDOWN | NETIF_MSG_TX_QUEUED;
module_param(msg_enable, int, 0000);
MODULE_PARM_DESC(msg_enable, "Override default message enable");

#define LAN743X_COMPONENT_FLAG_PCI          BIT(0)
#define LAN743X_COMPONENT_FLAG_CSR          BIT(1)
#define LAN743X_COMPONENT_FLAG_INTR         BIT(2)
#define LAN743X_COMPONENT_FLAG_DP           BIT(3)
#define LAN743X_COMPONENT_FLAG_GPIO         BIT(4)
#define LAN743X_COMPONENT_FLAG_MAC          BIT(5)
#define LAN743X_COMPONENT_FLAG_PHY          BIT(6)
#define LAN743X_COMPONENT_FLAG_PTP          BIT(7)
#define LAN743X_COMPONENT_FLAG_RFE          BIT(8)
#define LAN743X_COMPONENT_FLAG_FCT          BIT(9)
#define LAN743X_COMPONENT_FLAG_DMAC         BIT(10)
#define LAN743X_COMPONENT_FLAG_TX(channel)  BIT(16 + (channel))
#define LAN743X_COMPONENT_FLAG_RX(channel)  BIT(20 + (channel))

#define LAN743X_INIT_FLAG_NETDEV_REGISTERED BIT(24)

/* PCI */
#define INIT_FLAG_PCI_DEVICE_ENABLED        BIT(0)
#define INIT_FLAG_PCI_REGIONS_REQUESTED     BIT(1)
#define INIT_FLAG_CSR_MAPPED                BIT(2)

static int lan743x_pci_init(struct lan743x_adapter *adapter,
			    struct pci_dev *pdev)
{
	int ret = -ENODEV;
	int bars = 0;
	struct lan743x_pci *pci = &adapter->pci;

	NETIF_ASSERT(adapter, probe, adapter->netdev, pdev);
	memset(pci, 0, sizeof(struct lan743x_pci));
	pci->pdev = pdev;

	ret = pci_enable_device_mem(pdev);
	if (ret) {
		NETIF_WARNING(adapter, probe, adapter->netdev,
			      "failed pci_enable_device_mem, ret = %d", ret);
		goto clean_up;
	}
	pci->init_flags |= INIT_FLAG_PCI_DEVICE_ENABLED;

	if (pdev->vendor != PCI_VENDOR_ID_SMSC) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "Unsupported Vendor ID, 0x%04X,", pdev->vendor);
		ret = -ENODEV;
		goto clean_up;
	}

	if (pdev->device != PCI_DEVICE_ID_SMSC_LAN7430) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "Unsupported Device ID, 0x%04X", pdev->device);
		ret = -ENODEV;
		goto clean_up;
	}

	NETIF_INFO(adapter, probe, adapter->netdev,
		   "PCI: Vendor ID = 0x%04X, Device ID = 0x%04X",
		   pdev->vendor, pdev->device);

	bars = pci_select_bars(pdev, IORESOURCE_MEM);
	ret = pci_request_selected_regions(pdev, bars, DRIVER_NAME);
	if (ret) {
		NETIF_WARNING(adapter, probe, adapter->netdev,
			      "failed pci_request_selected_Regions, ret = %d",
			      ret);
		goto clean_up;
	}
	pci->init_flags |= INIT_FLAG_PCI_REGIONS_REQUESTED;
	pci->bar_flags = bars;

	pci_set_master(pdev);

clean_up:
	if (ret) {
		NETIF_WARNING(adapter, probe, adapter->netdev,
			      "pci init failed, performing cleanup");
		lan743x_pci_cleanup(adapter);
	}
	return ret;
}

static void lan743x_pci_cleanup(struct lan743x_adapter *adapter)
{
	struct lan743x_pci *pci = &adapter->pci;

	if (pci->init_flags & INIT_FLAG_PCI_REGIONS_REQUESTED) {
		pci_release_selected_regions(pci->pdev,
					     pci_select_bars(
					     pci->pdev, IORESOURCE_MEM));
		pci->init_flags &= ~INIT_FLAG_PCI_REGIONS_REQUESTED;
	}

	if (pci->init_flags & INIT_FLAG_PCI_DEVICE_ENABLED) {
		pci_disable_device(pci->pdev);
		pci->init_flags &= ~INIT_FLAG_PCI_DEVICE_ENABLED;
	}

	memset(pci, 0, sizeof(struct lan743x_pci));
}

static u8 __iomem *lan743x_pci_get_bar_address(struct lan743x_adapter *adapter,
					       int bar_index)
{
	u8 __iomem *result = NULL;
	struct lan743x_pci *pci = &adapter->pci;

	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     (bar_index >= 0) && (bar_index < 6));

	if (test_bit(bar_index, &pci->bar_flags)) {
		resource_size_t bar_start, bar_length;

		bar_start = pci_resource_start(pci->pdev, bar_index);
		bar_length = pci_resource_len(pci->pdev, bar_index);
		result = ioremap(bar_start, bar_length);
	}

	return result;
}

static void lan743x_pci_release_bar_address(struct lan743x_adapter *adapter,
					    int bar_index,
					    u8 __iomem *bar_address)
{
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     (bar_index >= 0) && (bar_index < 6));
	NETIF_ASSERT(adapter, drv, adapter->netdev, bar_address);

	iounmap(bar_address);
}

static unsigned int lan743x_pci_get_irq(struct lan743x_adapter *adapter)
{
	struct lan743x_pci *pci = &adapter->pci;

	return pci->pdev->irq;
}

/* CSR */
static int lan743x_csr_init(struct lan743x_adapter *adapter)
{
	struct lan743x_csr *csr = &adapter->csr;
	int result = -ENOMEM;
	int supported = 0;

	NETIF_ASSERT(adapter, probe, adapter->netdev, csr);
	memset(csr, 0, sizeof(struct lan743x_csr));

	csr->csr_address = lan743x_pci_get_bar_address(adapter, 0);
	if (!csr->csr_address) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "failed to get csr_address");
		result = -ENOMEM;
		goto clean_up;
	}

	csr->id_rev = lan743x_csr_read(adapter, ID_REV);
	csr->fpga_rev = lan743x_csr_read(adapter, FPGA_REV);

	NETIF_INFO(adapter, probe, adapter->netdev,
		   "ID_REV = 0x%08X, FPGA_REV = %d.%d",
		   csr->id_rev,	(csr->fpga_rev) & 0x000000FF,
		   ((csr->fpga_rev) >> 8) & 0x000000FF);

	if ((csr->id_rev & 0xFFFF0000) == 0x74300000)
		supported = 1;

	if (!supported) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "unsupported adapter, ID_REV = 0x%08X",
			    csr->id_rev);
		result = -ENODEV;
		goto clean_up;
	}

	result = lan743x_csr_light_reset(adapter);
	if (result) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "light reset failed");
		goto clean_up;
	}

	result = 0;

clean_up:
	if (result)
		lan743x_csr_cleanup(adapter);
	return result;
}

static void lan743x_csr_cleanup(struct lan743x_adapter *adapter)
{
	struct lan743x_csr *csr = &adapter->csr;

	if (csr->csr_address)
		lan743x_pci_release_bar_address(adapter, 0, csr->csr_address);

	memset(csr, 0, sizeof(struct lan743x_csr));
}

static int lan743x_csr_light_reset(struct lan743x_adapter *adapter)
{
	int result = -EIO;
	u32 data;
	unsigned long timeout;

	data = lan743x_csr_read(adapter, HW_CFG);
	data |= HW_CFG_LRST_;
	lan743x_csr_write(adapter, HW_CFG, data);

	timeout = jiffies + (10 * HZ);
	do {
		if (time_after(jiffies, timeout)) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "timeout, incomplete soft reset");
			result = -EIO;
			goto done;
		}
		msleep(100);
		data = lan743x_csr_read(adapter, HW_CFG);
	} while (data & HW_CFG_LRST_);
	result = 0;
done:
	return result;
}

static inline u32 lan743x_csr_read(struct lan743x_adapter *adapter, int offset)
{
	return ioread32(&adapter->csr.csr_address[offset]);
}

static inline void lan743x_csr_write(
	struct lan743x_adapter *adapter, int offset, u32 data)
{
	iowrite32(data, &adapter->csr.csr_address[offset]);
}

/* INTERRUPTS */
#define INTR_FLAG_IRQ_REQUESTED(vector_index)	BIT(0 + vector_index)
#define INTR_FLAG_MSI_ENABLED			BIT(8)
#define INTR_FLAG_MSIX_ENABLED			BIT(9)
#define INTR_FLAG_OPENED			BIT(10)

static void lan743x_vector_init(struct lan743x_vector *vector,
				struct lan743x_adapter *adapter,
				int vector_index, int irq, u32 int_mask,
				lan743x_vector_handler handler, void *context)
{
	NETIF_ASSERT(adapter, probe, adapter->netdev, vector);
	NETIF_ASSERT(adapter, probe, adapter->netdev, adapter);
	NETIF_ASSERT(adapter, probe, adapter->netdev, int_mask);
	NETIF_ASSERT(adapter, probe, adapter->netdev, handler);

	memset(vector, 0, sizeof(struct lan743x_vector));

	vector->adapter = adapter;
	vector->vector_index = vector_index;
	vector->irq = irq;
	vector->int_mask = int_mask;
	vector->handler = handler;
	vector->context = context;
}

static void lan743x_intr_software_isr(void *context)
{
	struct lan743x_adapter *adapter = (struct lan743x_adapter *)context;
	struct lan743x_intr *intr = &adapter->intr;
	u32 int_sts;

	NETIF_ASSERT(adapter, drv, adapter->netdev, adapter);

	int_sts = lan743x_csr_read(adapter, INT_STS);
	if (int_sts & INT_BIT_SW_GP_) {
		lan743x_csr_write(adapter, INT_STS, INT_BIT_SW_GP_);

		intr->software_isr_flag = 1;
	}
}

static void lan743x_intr_other_isr(void *context, u32 int_sts)
{
	struct lan743x_adapter *adapter = (struct lan743x_adapter *)context;

	NETIF_ASSERT(adapter, drv, adapter->netdev, adapter);

	if (int_sts & INT_BIT_ALL_OTHER_) {
		if (int_sts & INT_BIT_SW_GP_) {
			lan743x_intr_software_isr(adapter);
			int_sts &= ~INT_BIT_SW_GP_;
		}
		if (int_sts & INT_BIT_1588_) {
			lan743x_ptp_isr(adapter);
			int_sts &= ~INT_BIT_1588_;
		}
		if (int_sts & INT_BIT_MAC_) {
			lan743x_mac_isr(adapter);
			int_sts &= ~INT_BIT_MAC_;
		}
		if (int_sts & INT_BIT_FCT_) {
			lan743x_fct_isr(adapter);
			int_sts &= ~INT_BIT_FCT_;
		}
		if (int_sts & INT_BIT_DMA_GEN_) {
			lan743x_dmac_isr(adapter);
			int_sts &= ~INT_BIT_DMA_GEN_;
		}
	}
	if (int_sts) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "unhandled interrupt, int_sts = 0x%08X", int_sts);
		lan743x_csr_write(adapter, INT_EN_CLR, int_sts);
	}
}

static void lan743x_intr_union_isr(void *context, u32 int_sts)
{
	struct lan743x_adapter *adapter = (struct lan743x_adapter *)context;
	int channel;

	NETIF_ASSERT(adapter, drv, adapter->netdev, adapter);

	if (int_sts & INT_BIT_ALL_RX_) {
		for (channel = 0; channel < LAN743X_NUMBER_OF_RX_CHANNELS;
		     channel++) {
			u32 int_bit = INT_BIT_DMA_RX_(channel);

			if (int_sts & int_bit) {
				lan743x_rx_isr(&adapter->rx[channel],
					       int_bit);
				int_sts &= ~int_bit;
			}
		}
	}
	if (int_sts & INT_BIT_ALL_TX_) {
		for (channel = 0; channel < LAN743X_NUMBER_OF_TX_CHANNELS;
		     channel++) {
			u32 int_bit = INT_BIT_DMA_TX_(channel);

			if (int_sts & int_bit) {
				lan743x_tx_isr(&adapter->tx[channel],
					       int_bit);
				int_sts &= ~int_bit;
			}
		}
	}
	if (int_sts & INT_BIT_ALL_OTHER_) {
		lan743x_intr_other_isr(adapter, int_sts & INT_BIT_ALL_OTHER_);
		int_sts &= ~INT_BIT_ALL_OTHER_;
	}
	if (int_sts) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "unhandled interrupt, int_sts = 0x%08X", int_sts);
		lan743x_csr_write(adapter, INT_EN_CLR, int_sts);
	}
}

static irqreturn_t lan743x_vector_isr(int irq, void *ptr)
{
	irqreturn_t result = IRQ_NONE;
	struct lan743x_vector *vector = (struct lan743x_vector *)ptr;
	struct lan743x_adapter *adapter = NULL;
	u32 int_sts;
	u32 mask;

	NETIF_ASSERT(adapter, drv, adapter->netdev, vector);
	adapter = vector->adapter;
	NETIF_ASSERT(adapter, drv, adapter->netdev, adapter);

	int_sts = lan743x_csr_read(adapter, INT_STS);
	if (!(int_sts & INT_BIT_MAS_))
		goto irq_done;

	if (adapter->intr.number_of_vectors > 1) {
		NETIF_ASSERT(adapter, drv, adapter->netdev,
			     vector->vector_index >= 0);
		/* disable vector interrupt */
		lan743x_csr_write(adapter,
				  INT_VEC_EN_CLR,
				  INT_VEC_EN_(vector->vector_index));
	} else {
		/* disable master interrupt */
		lan743x_csr_write(adapter, INT_EN_CLR, INT_BIT_MAS_);
	}

	mask = lan743x_csr_read(adapter, INT_EN_SET);
	int_sts &= mask;

	int_sts &= (vector->int_mask);

	if (int_sts) {
		if (vector->handler) {
			vector->handler(vector->context, int_sts);
		} else {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "vector->handler == NULL");
			/* disable interrupts on this vector */
			lan743x_csr_write(adapter, INT_EN_CLR,
					  vector->int_mask);
		}
		result = IRQ_HANDLED;
	}

	if (adapter->intr.number_of_vectors > 1) {
		NETIF_ASSERT(adapter, drv, adapter->netdev,
			     vector->vector_index >= 0);
		/* enable vector interrupt */
		lan743x_csr_write(adapter,
				  INT_VEC_EN_SET,
				  INT_VEC_EN_(vector->vector_index));
	} else {
		/* enable master interrupt */
		lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_MAS_);
	}

irq_done:
	return result;
}

static int lan743x_intr_test_isr(struct lan743x_adapter *adapter)
{
	struct lan743x_intr *intr = &adapter->intr;
	int result = -ENODEV;
	int timeout = 10;

	intr->software_isr_flag = 0;

	/* enable interrupt */
	lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_SW_GP_);

	/* activate interrupt here */
	lan743x_csr_write(adapter, INT_SET, INT_BIT_SW_GP_);

	while ((timeout > 0) && (!(intr->software_isr_flag))) {
		usleep_range(1000, 20000);
		timeout--;
	}
	if (intr->software_isr_flag) {
		result = 0;
	} else {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "timed out while waiting for test interrupt");
	}

	/* disable interrupts */
	lan743x_csr_write(adapter, INT_EN_CLR, INT_BIT_SW_GP_);

	return result;
}

static int lan743x_intr_init(struct lan743x_adapter *adapter)
{
	struct lan743x_intr *intr = &adapter->intr;

	memset(intr, 0, sizeof(struct lan743x_intr));

	intr->irq = lan743x_pci_get_irq(adapter);

	lan743x_csr_write(adapter, INT_EN_CLR, 0xFFFFFFFF);

	return 0;
}

static void lan743x_intr_cleanup(struct lan743x_adapter *adapter)
{
	struct lan743x_intr *intr = &adapter->intr;

	lan743x_csr_write(adapter, INT_EN_CLR, 0xFFFFFFFF);

	memset(intr, 0, sizeof(struct lan743x_intr));
}

static int lan743x_intr_open(struct lan743x_adapter *adapter)
{
	int ret = -ENODEV;
	struct lan743x_intr *intr = &adapter->intr;
	int index = 0;

	NETIF_ASSERT(adapter, ifup, adapter->netdev,
		     !(intr->flags & INTR_FLAG_OPENED));

	intr->number_of_vectors = 0;

	memset(&intr->msix_entries[0], 0,
	       sizeof(struct msix_entry) * LAN743X_MAX_VECTOR_COUNT);
	for (index = 0; index < LAN743X_MAX_VECTOR_COUNT; index++)
		intr->msix_entries[index].entry = index;

	ret = pci_enable_msix_range(adapter->pci.pdev,
				    intr->msix_entries,
				    LAN743X_MAX_VECTOR_COUNT,
				    LAN743X_MAX_VECTOR_COUNT);
	if (ret > 0) {
		intr->flags |= INTR_FLAG_MSIX_ENABLED;
		NETIF_INFO(adapter, ifup, adapter->netdev,
			   "Using MSIX interrupt mode");
		if (ret == LAN743X_MAX_VECTOR_COUNT) {
			lan743x_vector_init(&intr->vector_list[0], adapter,
					    0, intr->msix_entries[0].vector,
					    INT_BIT_DMA_RX_(0),
					    lan743x_rx_isr, &adapter->rx[0]);

			ret = request_irq(intr->vector_list[0].irq,
					  lan743x_vector_isr, 0,
					  DRIVER_NAME,
					  &intr->vector_list[0]);
			if (ret) {
				NETIF_ERROR(adapter, ifup, adapter->netdev,
					    "request_irq failed, ret = %d",
					    ret);
				goto clean_up;
			}
			intr->flags |= INTR_FLAG_IRQ_REQUESTED(0);

			lan743x_vector_init(&intr->vector_list[4], adapter,
					    4, intr->msix_entries[4].vector,
					    INT_BIT_DMA_TX_(0),
					    lan743x_tx_isr, &adapter->tx[0]);

			ret = request_irq(intr->vector_list[4].irq,
					  lan743x_vector_isr, 0,
					  DRIVER_NAME,
					  &intr->vector_list[4]);
			if (ret) {
				NETIF_ERROR(adapter, ifup, adapter->netdev,
					    "request_irq failed, ret = %d",
					    ret);
				goto clean_up;
			}
			intr->flags |= INTR_FLAG_IRQ_REQUESTED(4);

			lan743x_vector_init(&intr->vector_list[5], adapter,
					    5, intr->msix_entries[5].vector,
					    INT_BIT_ALL_OTHER_,
					    lan743x_intr_other_isr, adapter);

			ret = request_irq(intr->vector_list[5].irq,
					  lan743x_vector_isr, 0,
					  DRIVER_NAME,
					  &intr->vector_list[5]);
			if (ret) {
				NETIF_ERROR(adapter, ifup, adapter->netdev,
					    "request_irq failed, ret = %d",
					    ret);
				goto clean_up;
			}
			intr->flags |= INTR_FLAG_IRQ_REQUESTED(5);
			intr->number_of_vectors = 3;

			/* map all interrupts */
			lan743x_csr_write(adapter, INT_VEC_MAP0, 0);
			lan743x_csr_write(adapter, INT_VEC_MAP1, 4);
			lan743x_csr_write(adapter, INT_VEC_MAP2, 0x00555555);

			/* enable vector 0, 4, 5 */
			lan743x_csr_write(adapter, INT_VEC_EN_SET,
					  INT_VEC_EN_(0));
			lan743x_csr_write(adapter, INT_VEC_EN_SET,
					  INT_VEC_EN_(4));
			lan743x_csr_write(adapter, INT_VEC_EN_SET,
					  INT_VEC_EN_(5));

			/* enable interrupts */
			lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_MAS_);

			ret = lan743x_intr_test_isr(adapter);
			if (ret) {
				NETIF_ERROR(adapter, ifup, adapter->netdev,
					    "ISR test failed, irq = %d",
					    intr->vector_list[5].irq);
				goto clean_up;
			} else {
				NETIF_INFO(adapter, ifup, adapter->netdev,
					   "irq = %d, passed ISR Test",
					   intr->vector_list[5].irq);
			}
		} else {
			if (ret != LAN743X_MAX_VECTOR_COUNT) {
				NETIF_WARNING(adapter, ifup, adapter->netdev,
					      "pci_enable_msix_range returned %d, but requested %d MSIX vectors",
					      ret, LAN743X_MAX_VECTOR_COUNT);
				NETIF_WARNING(adapter, ifup, adapter->netdev,
					      "Will use only 1 MSIX vector instead");
			}

			lan743x_vector_init(&intr->vector_list[0], adapter,
					    0, intr->msix_entries[0].vector,
					    INT_BIT_ALL_RX_ | INT_BIT_ALL_TX_ |
					    INT_BIT_ALL_OTHER_,
					    lan743x_intr_union_isr, adapter);

			ret = request_irq(intr->vector_list[0].irq,
					  lan743x_vector_isr, 0,
					  DRIVER_NAME,
					  &intr->vector_list[0]);
			if (ret) {
				NETIF_ERROR(adapter, ifup, adapter->netdev,
					    "request_irq failed, ret = %d",
					    ret);
				goto clean_up;
			}
			intr->flags |= INTR_FLAG_IRQ_REQUESTED(0);
			intr->number_of_vectors = 1;

			/* map all interrupts to vector 0 */
			lan743x_csr_write(adapter, INT_VEC_MAP0, 0);
			lan743x_csr_write(adapter, INT_VEC_MAP1, 0);
			lan743x_csr_write(adapter, INT_VEC_MAP2, 0);

			/* enable vector 0 */
			lan743x_csr_write(adapter, INT_VEC_EN_SET,
					  INT_VEC_EN_(0));

			/* enable interrupts */
			lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_MAS_);

			ret = lan743x_intr_test_isr(adapter);
			if (ret) {
				NETIF_ERROR(adapter, ifup, adapter->netdev,
					    "ISR test failed, irq = %d",
					    intr->vector_list[0].irq);
				goto clean_up;
			} else {
				NETIF_INFO(adapter, ifup, adapter->netdev,
					   "irq = %d, passed ISR Test",
					   intr->vector_list[0].irq);
			}
		}
	} else if (!pci_enable_msi(adapter->pci.pdev)) {
		intr->flags |= INTR_FLAG_MSI_ENABLED;
		NETIF_INFO(adapter, ifup, adapter->netdev,
			   "Using MSI interrupt mode");

		lan743x_vector_init(&intr->vector_list[0], adapter,
				    0, adapter->pci.pdev->irq,
				    INT_BIT_ALL_RX_ | INT_BIT_ALL_TX_ |
				    INT_BIT_ALL_OTHER_,
				    lan743x_intr_union_isr, adapter);

		ret = request_irq(intr->vector_list[0].irq, lan743x_vector_isr,
				  0, DRIVER_NAME, &intr->vector_list[0]);
		if (ret) {
			NETIF_ERROR(adapter, ifup, adapter->netdev,
				    "request_irq failed, ret = %d", ret);
			goto clean_up;
		}
		intr->flags |= INTR_FLAG_IRQ_REQUESTED(0);
		intr->number_of_vectors = 1;

		/* map all interrupts to vector 0 */
		lan743x_csr_write(adapter, INT_VEC_MAP0, 0);
		lan743x_csr_write(adapter, INT_VEC_MAP1, 0);
		lan743x_csr_write(adapter, INT_VEC_MAP2, 0);

		/* enable vector 0 */
		lan743x_csr_write(adapter, INT_VEC_EN_SET, INT_VEC_EN_(0));

		/* enable interrupts */
		lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_MAS_);

		ret = lan743x_intr_test_isr(adapter);
		if (ret) {
			NETIF_ERROR(adapter, ifup, adapter->netdev,
				    "ISR test failed, irq = %d",
				    intr->vector_list[0].irq);
			goto clean_up;
		} else {
			NETIF_INFO(adapter, ifup, adapter->netdev,
				   "irq = %d, passed ISR Test",
				   intr->vector_list[0].irq);
		}
	} else {
		NETIF_INFO(adapter, ifup, adapter->netdev,
			   "Using legacy interrupt mode");

		lan743x_vector_init(&intr->vector_list[0], adapter,
				    -1, intr->irq,
				    INT_BIT_ALL_RX_ | INT_BIT_ALL_TX_ |
				    INT_BIT_ALL_OTHER_,
				    lan743x_intr_union_isr, adapter);

		ret = request_irq(intr->vector_list[0].irq, lan743x_vector_isr,
				  IRQF_SHARED, DRIVER_NAME,
				  &intr->vector_list[0]);
		if (ret) {
			NETIF_ERROR(adapter, ifup, adapter->netdev,
				    "request_irq failed, ret = %d", ret);
			goto clean_up;
		}
		intr->flags |= INTR_FLAG_IRQ_REQUESTED(0);
		intr->number_of_vectors = 1;

		/* enable interrupts */
		lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_MAS_);

		ret = lan743x_intr_test_isr(adapter);
		if (ret) {
			NETIF_ERROR(adapter, ifup, adapter->netdev,
				    "ISR test failed, irq = %d",
				    intr->vector_list[0].irq);
			goto clean_up;
		} else {
			NETIF_INFO(adapter, ifup, adapter->netdev,
				   "irq = %d, passed ISR Test",
				   intr->vector_list[0].irq);
		}
	}

	intr->flags |= INTR_FLAG_OPENED;
	ret = 0;

clean_up:
	if (ret)
		lan743x_intr_close(adapter);
	return ret;
}

static void lan743x_intr_close(struct lan743x_adapter *adapter)
{
	struct lan743x_intr *intr = &adapter->intr;
	int index = 0;

	intr->flags &= ~INTR_FLAG_OPENED;

	lan743x_csr_write(adapter, INT_EN_CLR, INT_BIT_MAS_);
	lan743x_csr_write(adapter, INT_VEC_EN_CLR, 0x000000FF);

	for (index = 0; index < LAN743X_MAX_VECTOR_COUNT; index++) {
		if ((intr->flags) & INTR_FLAG_IRQ_REQUESTED(index)) {
			free_irq(intr->vector_list[index].irq,
				 &intr->vector_list[index]);
			intr->flags &= ~INTR_FLAG_IRQ_REQUESTED(index);
		}
	}
	if ((intr->flags) & INTR_FLAG_MSI_ENABLED) {
		pci_disable_msi(adapter->pci.pdev);
		intr->flags &= ~INTR_FLAG_MSI_ENABLED;
	}
	if ((intr->flags) & INTR_FLAG_MSIX_ENABLED) {
		pci_disable_msix(adapter->pci.pdev);
		intr->flags &= ~INTR_FLAG_MSIX_ENABLED;
	}
}

/* DP */
static int lan743x_dp_init(struct lan743x_adapter *adapter)
{
	struct lan743x_dp *dp = &adapter->dp;

	NETIF_ASSERT(adapter, probe, adapter->netdev, dp);
	memset(dp, 0, sizeof(*dp));

	mutex_init(&dp->lock);

	return 0;
}

static void lan743x_dp_cleanup(struct lan743x_adapter *adapter)
{
	struct lan743x_dp *dp = &adapter->dp;

	memset(dp, 0, sizeof(*dp));
}

static int lan743x_dp_open(struct lan743x_adapter *adapter)
{
	/* This empty function is kept as a place holder */
	return 0;
}

static void lan743x_dp_close(struct lan743x_adapter *adapter)
{
	/* This empty function is kept as a place holder */
}

static int lan743x_dp_wait_till_not_busy(struct lan743x_adapter *adapter)
{
	int i;
	u32 dp_sel = 0;

	for (i = 0; i < 100; i++) {
		dp_sel = lan743x_csr_read(adapter, DP_SEL);
		if (dp_sel & DP_SEL_DPRDY_)
			return 0;
		usleep_range(40, 100);
	}
	NETIF_ERROR(adapter, drv, adapter->netdev,
		    "Timed out waiting for data port not busy");
	return -EIO;
}

static int lan743x_dp_write(struct lan743x_adapter *adapter,
			    u32 select, u32 addr, u32 length, u32 *buf)
{
	struct lan743x_dp *dp = &adapter->dp;
	int ret = -EIO;
	int i;
	u32 dp_sel;

	NETIF_ASSERT(adapter, drv, adapter->netdev, buf);

	mutex_lock(&dp->lock);

	if (lan743x_dp_wait_till_not_busy(adapter))
		goto done;

	dp_sel = lan743x_csr_read(adapter, DP_SEL);
	dp_sel &= ~DP_SEL_MASK_;
	dp_sel |= select;
	lan743x_csr_write(adapter, DP_SEL, dp_sel);

	for (i = 0; i < length; i++) {
		lan743x_csr_write(adapter, DP_ADDR, addr + i);
		lan743x_csr_write(adapter, DP_DATA_0, buf[i]);
		lan743x_csr_write(adapter, DP_CMD, DP_CMD_WRITE_);
		if (lan743x_dp_wait_till_not_busy(adapter))
			goto done;
	}
	ret = 0;

done:
	mutex_unlock(&dp->lock);
	return ret;
}

static int lan743x_dp_write_hash_filter(struct lan743x_adapter *adapter,
					u32 *hash_data)
{
	NETIF_ASSERT(adapter, drv, adapter->netdev, hash_data);

	return lan743x_dp_write(adapter, DP_SEL_RFE_RAM,
				DP_SEL_VHF_VLAN_LEN,
				DP_SEL_VHF_HASH_LEN, hash_data);
}

/* GPIO */
#define LAN743X_NUMBER_OF_GPIO          (12)

static int lan743x_gpio_init(struct lan743x_adapter *adapter)
{
	struct lan743x_gpio *gpio = &adapter->gpio;

	NETIF_ASSERT(adapter, probe, adapter->netdev, gpio);
	memset(gpio, 0, sizeof(*gpio));

	spin_lock_init(&gpio->gpio_lock);

	gpio->gpio_cfg0 = 0; /* set all direction to input, data = 0 */
	gpio->gpio_cfg1 = 0x0FFF0000;/* disable all gpio, set to open drain */
	gpio->gpio_cfg2 = 0;/* set all to 1588 low polarity level */
	gpio->gpio_cfg3 = 0;/* disable all 1588 output */
	lan743x_csr_write(adapter, GPIO_CFG0, gpio->gpio_cfg0);
	lan743x_csr_write(adapter, GPIO_CFG1, gpio->gpio_cfg1);
	lan743x_csr_write(adapter, GPIO_CFG2, gpio->gpio_cfg2);
	lan743x_csr_write(adapter, GPIO_CFG3, gpio->gpio_cfg3);

	return 0;
}

static void lan743x_gpio_cleanup(struct lan743x_adapter *adapter)
{
	struct lan743x_gpio *gpio = &adapter->gpio;

	memset(gpio, 0, sizeof(*gpio));
}

static int lan743x_gpio_open(struct lan743x_adapter *adapter)
{
	/* This empty function is kept as a place holder */
	return 0;
}

static void lan743x_gpio_close(struct lan743x_adapter *adapter)
{
	/* This empty function is kept as a place holder */
}

#ifdef CONFIG_PTP_1588_CLOCK
static int lan743x_gpio_reserve_ptp_output(struct lan743x_adapter *adapter,
					   int bit, int ptp_channel)
{
	struct lan743x_gpio *gpio = &adapter->gpio;
	int ret = -EBUSY;
	unsigned long irq_flags = 0;
	int bit_mask = BIT(bit);

	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     (bit >= 0) && (bit < LAN743X_NUMBER_OF_GPIO));
	spin_lock_irqsave(&gpio->gpio_lock, irq_flags);

	if (!((gpio->used_bits) & bit_mask)) {
		gpio->used_bits |= bit_mask;
		gpio->output_bits |= bit_mask;
		gpio->ptp_bits |= bit_mask;

		/* set as output, and zero initial value */
		gpio->gpio_cfg0 |= GPIO_CFG0_GPIO_DIR_(bit);
		gpio->gpio_cfg0 &= ~GPIO_CFG0_GPIO_DATA_(bit);
		lan743x_csr_write(adapter, GPIO_CFG0, gpio->gpio_cfg0);

		/* enable gpio , and set buffer type to push pull */
		gpio->gpio_cfg1 &= ~GPIO_CFG1_GPIOEN_(bit);
		gpio->gpio_cfg1 |= GPIO_CFG1_GPIOBUF_(bit);
		lan743x_csr_write(adapter, GPIO_CFG1, gpio->gpio_cfg1);

		/* set 1588 polarity to high */
		gpio->gpio_cfg2 |= GPIO_CFG2_1588_POL_(bit);
		lan743x_csr_write(adapter, GPIO_CFG2, gpio->gpio_cfg2);

		if (!ptp_channel) {
			/* use channel A */
			gpio->gpio_cfg3 &= ~GPIO_CFG3_1588_CH_SEL_(bit);
		} else {
			/* use channel B */
			gpio->gpio_cfg3 |= GPIO_CFG3_1588_CH_SEL_(bit);
		}
		gpio->gpio_cfg3 |= GPIO_CFG3_1588_OE_(bit);
		lan743x_csr_write(adapter, GPIO_CFG3, gpio->gpio_cfg3);

		ret = bit;
	} else {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "GPIO %d is already in use", bit);
	}
	spin_unlock_irqrestore(&gpio->gpio_lock, irq_flags);
	return ret;
}
#endif /* CONFIG_PTP_1588_CLOCK */

#ifdef CONFIG_PTP_1588_CLOCK
static void lan743x_gpio_release(struct lan743x_adapter *adapter, int bit)
{
	struct lan743x_gpio *gpio = &adapter->gpio;
	unsigned long irq_flags = 0;
	int bit_mask = BIT(bit);

	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     (bit >= 0) && (bit < LAN743X_NUMBER_OF_GPIO));
	spin_lock_irqsave(&gpio->gpio_lock, irq_flags);
	if (gpio->used_bits & bit_mask) {
		gpio->used_bits &= ~bit_mask;
		if (gpio->output_bits & bit_mask) {
			gpio->output_bits &= ~bit_mask;

			if ((gpio->ptp_bits) & bit_mask) {
				gpio->ptp_bits &= ~bit_mask;
				/* disable ptp output */
				gpio->gpio_cfg3 &= ~GPIO_CFG3_1588_OE_(bit);
				lan743x_csr_write(adapter, GPIO_CFG3,
						  gpio->gpio_cfg3);
			}
			/* release gpio output */

			/* disable gpio */
			gpio->gpio_cfg1 |= GPIO_CFG1_GPIOEN_(bit);
			gpio->gpio_cfg1 &= ~GPIO_CFG1_GPIOBUF_(bit);
			lan743x_csr_write(adapter, GPIO_CFG1, gpio->gpio_cfg1);

			/* reset back to input */
			gpio->gpio_cfg0 &= ~GPIO_CFG0_GPIO_DIR_(bit);
			gpio->gpio_cfg0 &= ~GPIO_CFG0_GPIO_DATA_(bit);
			lan743x_csr_write(adapter, GPIO_CFG0, gpio->gpio_cfg0);
		} else {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Not Implemented, release gpio input");
		}
	} else {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "GPIO %d is not used", bit);
	}
	spin_unlock_irqrestore(&gpio->gpio_lock, irq_flags);
}
#endif /* CONFIG_PTP_1588_CLOCK */

/* PTP */
#define LAN743X_PTP_MAX_FREQ_ADJ_IN_PPB (31249999)

#define LAN743X_PTPCI_TO_PTP    \
	(container_of(ptpci, struct lan743x_ptp, ptp_clock_info))
#define LAN743X_PTP_TO_ADAPTER  \
	(container_of(ptp, struct lan743x_adapter, ptp))

#ifdef CONFIG_PTP_1588_CLOCK
static int lan743x_ptp_reserve_event_ch(struct lan743x_adapter *adapter);
static void lan743x_ptp_release_event_ch(struct lan743x_adapter *adapter,
					 int event_channel);
#endif

#ifdef CONFIG_PTP_1588_CLOCK
static int lan743x_ptpci_adjfreq(struct ptp_clock_info *ptpci, s32 delta_ppb)
{
	u32 u32_delta = 0;
	u64 u64_delta = 0;
	u32 lan743x_rate_adj = 0;
	bool positive = true;
	struct lan743x_ptp *ptp = LAN743X_PTPCI_TO_PTP;
	struct lan743x_adapter *adapter = LAN743X_PTP_TO_ADAPTER;

	if ((delta_ppb < (-LAN743X_PTP_MAX_FREQ_ADJ_IN_PPB)) ||
	    (delta_ppb > LAN743X_PTP_MAX_FREQ_ADJ_IN_PPB)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "delta_ppb = %d, out of range", delta_ppb);
		return -EINVAL;
	}
	if (delta_ppb > 0) {
		u32_delta = (u32)delta_ppb;
		positive = true;
	} else {
		u32_delta = (u32)(-delta_ppb);
		positive = false;
	}
	u64_delta = (((u64)u32_delta) * 0x800000000ULL);
	lan743x_rate_adj = (u32)(u64_delta / 1000000000);
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     !(lan743x_rate_adj &
		     (~PTP_CLOCK_RATE_ADJ_VALUE_MASK_)));

	if (positive)
		lan743x_rate_adj |= PTP_CLOCK_RATE_ADJ_DIR_;

	lan743x_csr_write(LAN743X_PTP_TO_ADAPTER, PTP_CLOCK_RATE_ADJ,
			  lan743x_rate_adj);

	NETIF_INFO(adapter, drv, adapter->netdev,
		   "adjfreq, delta_ppb = %d, lan743x_rate_adj = 0x%08X",
		   delta_ppb, lan743x_rate_adj);
	return 0;
}
#endif /*CONFIG_PTP_1588_CLOCK */

#ifdef CONFIG_PTP_1588_CLOCK
static int lan743x_ptpci_adjtime(struct ptp_clock_info *ptpci, s64 delta)
{
	struct lan743x_ptp *ptp = LAN743X_PTPCI_TO_PTP;
	struct lan743x_adapter *adapter = LAN743X_PTP_TO_ADAPTER;

	lan743x_ptp_clock_step(LAN743X_PTP_TO_ADAPTER, delta);
	NETIF_INFO(adapter, drv, adapter->netdev,
		   "adjtime, delta = %lld", delta);
	return 0;
}
#endif /*CONFIG_PTP_1588_CLOCK */

#ifdef CONFIG_PTP_1588_CLOCK
static int lan743x_ptpci_gettime64(struct ptp_clock_info *ptpci,
				   struct timespec64 *ts)
{
	struct lan743x_ptp *ptp = LAN743X_PTPCI_TO_PTP;
	struct lan743x_adapter *adapter = LAN743X_PTP_TO_ADAPTER;

	if (ts) {
		u32 seconds = 0;
		u32 nano_seconds = 0;

		lan743x_ptp_clock_get(adapter, &seconds, &nano_seconds, NULL);
		ts->tv_sec = seconds;
		ts->tv_nsec = nano_seconds;
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "gettime = %u.%09u", seconds, nano_seconds);
	} else {
		NETIF_WARNING(adapter, drv, adapter->netdev, "ts == NULL");
		return -EINVAL;
	}
	return 0;
}
#endif /*CONFIG_PTP_1588_CLOCK */

#ifdef CONFIG_PTP_1588_CLOCK
static int lan743x_ptpci_settime64(struct ptp_clock_info *ptpci,
				   const struct timespec64 *ts)
{
	struct lan743x_ptp *ptp = LAN743X_PTPCI_TO_PTP;
	struct lan743x_adapter *adapter = LAN743X_PTP_TO_ADAPTER;

	if (ts) {
		u32 seconds = 0;
		u32 nano_seconds = 0;

		if ((ts->tv_sec > 0xFFFFFFFFLL) ||
		    (ts->tv_sec < 0)) {
			NETIF_WARNING(adapter, drv, adapter->netdev,
				      "ts->tv_sec out of range, %ld",
				      ts->tv_sec);
			return -EINVAL;
		}
		if ((ts->tv_nsec >= 1000000000L) ||
		    (ts->tv_nsec < 0)) {
			NETIF_WARNING(adapter, drv, adapter->netdev,
				      "ts->tv_nsec out of range, %ld",
				      ts->tv_nsec);
			return -EINVAL;
		}
		seconds = ts->tv_sec;
		nano_seconds = ts->tv_nsec;
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "settime = %u.%09u", seconds, nano_seconds);
		lan743x_ptp_clock_set(adapter, seconds, nano_seconds, 0);
	} else {
		NETIF_WARNING(adapter, drv, adapter->netdev, "ts == NULL");
		return -EINVAL;
	}
	return 0;
}
#endif /*CONFIG_PTP_1588_CLOCK */

#ifdef CONFIG_PTP_1588_CLOCK
static int lan743x_ptp_enable_pps(struct lan743x_adapter *adapter)
{
	struct lan743x_ptp *ptp = &adapter->ptp;
	int result = -ENODEV;
	u32 current_seconds = 0;
	u32 target_seconds = 0;
	u32 general_config = 0;

	if (ptp->pps_event_ch >= 0) {
		NETIF_INFO(adapter, drv, adapter->netdev, "PPS already ON");
		result = 0;
		goto done;
	}

	ptp->pps_event_ch = lan743x_ptp_reserve_event_ch(adapter);
	if (ptp->pps_event_ch < 0) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Failed to reserve event channel for PPS");
		goto done;
	}

	NETIF_ASSERT(adapter, drv, adapter->netdev, ptp->pps_gpio_bit < 0);

	ptp->pps_gpio_bit = lan743x_gpio_reserve_ptp_output(adapter, 0,
							    ptp->pps_event_ch);

	if (ptp->pps_gpio_bit < 0) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Failed to reserve gpio 0 for PPS");
		goto done;
	}

	lan743x_ptp_clock_get(adapter, &current_seconds, NULL, NULL);

	/* set the first target ahead by 2 seconds
	 *	to make sure its not missed
	 */
	target_seconds = current_seconds + 2;

	/* set the new target */
	lan743x_csr_write(adapter,
			  PTP_CLOCK_TARGET_SEC_X(ptp->pps_event_ch),
			  0xFFFF0000);
	lan743x_csr_write(adapter,
			  PTP_CLOCK_TARGET_NS_X(ptp->pps_event_ch), 0);

	general_config = lan743x_csr_read(adapter,
					  PTP_GENERAL_CONFIG);

	general_config &= ~(PTP_GENERAL_CONFIG_CLOCK_EVENT_X_MASK_(
			  ptp->pps_event_ch));
	general_config |= PTP_GENERAL_CONFIG_CLOCK_EVENT_X_SET_(
			  ptp->pps_event_ch,
			  PTP_GENERAL_CONFIG_CLOCK_EVENT_100US_);
	general_config &= ~PTP_GENERAL_CONFIG_RELOAD_ADD_X_(
			  ptp->pps_event_ch);
	lan743x_csr_write(adapter, PTP_GENERAL_CONFIG, general_config);

	/* set the reload to one second steps */
	lan743x_csr_write(adapter,
			  PTP_CLOCK_TARGET_RELOAD_SEC_X(ptp->pps_event_ch),
			  1);
	lan743x_csr_write(adapter,
			  PTP_CLOCK_TARGET_RELOAD_NS_X(ptp->pps_event_ch),
			  0);

	/* set the new target */
	lan743x_csr_write(adapter,
			  PTP_CLOCK_TARGET_SEC_X(ptp->pps_event_ch),
			  target_seconds);
	lan743x_csr_write(adapter,
			  PTP_CLOCK_TARGET_NS_X(ptp->pps_event_ch),
			  0);

	NETIF_INFO(adapter, drv, adapter->netdev,
		   "PPS enabled, channel = %d, gpio = %d",
		   ptp->pps_event_ch, ptp->pps_gpio_bit);

	result = 0;
done:
	if (result < 0) {
		if (ptp->pps_gpio_bit >= 0) {
			lan743x_gpio_release(adapter, ptp->pps_gpio_bit);
			ptp->pps_gpio_bit = -1;
		}
		if (ptp->pps_event_ch >= 0) {
			lan743x_ptp_release_event_ch(adapter,
						     ptp->pps_event_ch);
			ptp->pps_event_ch = -1;
		}
	}
	return result;
}
#endif /* CONFIG_PTP_1588_CLOCK */

#ifdef CONFIG_PTP_1588_CLOCK
static void lan743x_ptp_disable_pps(struct lan743x_adapter *adapter)
{
	struct lan743x_ptp *ptp = &adapter->ptp;

	if (ptp->pps_gpio_bit >= 0) {
		lan743x_gpio_release(adapter, ptp->pps_gpio_bit);
		ptp->pps_gpio_bit = -1;
	}

	if (ptp->pps_event_ch >= 0) {
		u32 general_config = 0;

		/* set target to far in the future, effectively disabling it */
		lan743x_csr_write(adapter,
				  PTP_CLOCK_TARGET_SEC_X(ptp->pps_event_ch),
				  0xFFFF0000);
		lan743x_csr_write(adapter,
				  PTP_CLOCK_TARGET_NS_X(ptp->pps_event_ch), 0);

		general_config = lan743x_csr_read(adapter, PTP_GENERAL_CONFIG);
		general_config |= PTP_GENERAL_CONFIG_RELOAD_ADD_X_(
				  ptp->pps_event_ch);
		lan743x_csr_write(adapter, PTP_GENERAL_CONFIG, general_config);
		lan743x_ptp_release_event_ch(adapter, ptp->pps_event_ch);
		ptp->pps_event_ch = -1;
	}
}
#endif /* CONFIG_PTP_1588_CLOCK */

#ifdef CONFIG_PTP_1588_CLOCK
static int lan743x_ptpci_enable(struct ptp_clock_info *ptpci,
				struct ptp_clock_request *request, int on)
{
	struct lan743x_ptp *ptp = LAN743X_PTPCI_TO_PTP;
	struct lan743x_adapter *adapter = NULL;

	adapter = LAN743X_PTP_TO_ADAPTER;
	if (request) {
		switch (request->type) {
		case PTP_CLK_REQ_EXTTS:
			NETIF_INFO(adapter, drv, adapter->netdev,
				   "request->type == PTP_CLK_REQ_EXTTS");
			NETIF_INFO(adapter, drv, adapter->netdev,
				   "request->extts.index = %d",
				   request->extts.index);
			NETIF_INFO(adapter, drv, adapter->netdev,
				   "request->extts.flags = 0x%08X",
				   request->extts.flags);
			NETIF_INFO(adapter, drv, adapter->netdev,
				   "on = %d", on);
			return -EINVAL;
		case PTP_CLK_REQ_PEROUT:
			NETIF_INFO(adapter, drv, adapter->netdev,
				   "request->type == PTP_CLK_REQ_PEROUT");
			NETIF_INFO(adapter, drv, adapter->netdev,
				   "on = %d", on);
			{
				NETIF_INFO(adapter, drv, adapter->netdev,
					   "  start = %lld.%09u",
					   request->perout.start.sec,
					   request->perout.start.nsec);
				NETIF_INFO(adapter, drv, adapter->netdev,
					   "  period = %lld.%09u",
					   request->perout.period.sec,
					   request->perout.period.nsec);
				NETIF_INFO(adapter, drv, adapter->netdev,
					   "  index = %u",
					   request->perout.index);
			}
			return -EINVAL;
		case PTP_CLK_REQ_PPS:
			if (on) {
				if (lan743x_ptp_enable_pps(adapter) >= 0) {
					NETIF_INFO(adapter, drv,
						   adapter->netdev,
						   "PPS is ON");
				} else {
					NETIF_WARNING(adapter, drv,
						      adapter->netdev,
						      "Error starting PPS");
				}
			} else {
				lan743x_ptp_disable_pps(adapter);
				NETIF_INFO(adapter, drv, adapter->netdev,
					   "PPS is OFF");
			}
			break;
		default:
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "request->type == %d, Unknown",
				    request->type);
			break;
		}
	} else {
		NETIF_ERROR(adapter, drv, adapter->netdev, "request == NULL");
	}
	return 0;
}
#endif /* CONFIG_PTP_1588_CLOCK */

static void lan743x_ptp_isr(void *context)
{
	int enable_flag = 1;
	u32 ptp_int_sts = 0;
	struct lan743x_adapter *adapter = (struct lan743x_adapter *)context;
	struct lan743x_ptp *ptp = NULL;

	NETIF_ASSERT(adapter, drv, adapter->netdev, adapter);
	ptp = &adapter->ptp;

	lan743x_csr_write(adapter, INT_EN_CLR, INT_BIT_1588_);

	ptp_int_sts = lan743x_csr_read(adapter, PTP_INT_STS);
	if (ptp_int_sts & PTP_INT_BIT_TX_TS_) {
		tasklet_schedule(&ptp->ptp_isr_bottom_half);
		enable_flag = 0;/* tasklet will re-enable later */
	}
	if (ptp_int_sts & PTP_INT_BIT_TX_SWTS_ERR_) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "PTP TX Software Timestamp Error");
		/* clear int status bit */
		lan743x_csr_write(adapter, PTP_INT_STS,
				  PTP_INT_BIT_TX_SWTS_ERR_);
	}
	if (ptp_int_sts & PTP_INT_BIT_TIMER_B_) {
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "PTP TIMER B Interrupt");
		/* clear int status bit */
		lan743x_csr_write(adapter, PTP_INT_STS,
				  PTP_INT_BIT_TIMER_B_);
	}
	if (ptp_int_sts & PTP_INT_BIT_TIMER_A_) {
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "PTP TIMER A Interrupt");
		/* clear int status bit */
		lan743x_csr_write(adapter, PTP_INT_STS,
				  PTP_INT_BIT_TIMER_A_);
	}

	if (enable_flag) {
		/* re-enable isr */
		lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_1588_);
	}
}

static void lan743x_ptp_tx_ts_complete(struct lan743x_adapter *adapter)
{
	struct lan743x_ptp *ptp = &adapter->ptp;
	int i;
	int c;

	mutex_lock(&ptp->tx_ts_lock);
	c = ptp->tx_ts_skb_queue_size;

	if (c > ptp->tx_ts_queue_size)
		c = ptp->tx_ts_queue_size;
	if (c <= 0)
		goto done;

	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     c <=
		     LAN743X_PTP_NUMBER_OF_TX_TIMESTAMPS);
	for (i = 0; i < c; i++) {
		struct skb_shared_hwtstamps tstamps;
		struct sk_buff *skb = ptp->tx_ts_skb_queue[i];
		u32 seconds = ptp->tx_ts_seconds_queue[i];
		u32 nseconds = ptp->tx_ts_nseconds_queue[i];

		NETIF_ASSERT(adapter, drv,
			     adapter->netdev, skb);

		memset(&tstamps, 0, sizeof(tstamps));
		tstamps.hwtstamp = ktime_set(seconds, nseconds);
		skb_tstamp_tx(skb, &tstamps);
		dev_kfree_skb(skb);

		ptp->tx_ts_skb_queue[i] = NULL;
		ptp->tx_ts_seconds_queue[i] = 0;
		ptp->tx_ts_nseconds_queue[i] = 0;
	}

	/* shift queue */
	for (i = c;
	     i < LAN743X_PTP_NUMBER_OF_TX_TIMESTAMPS;
	     i++) {
		ptp->tx_ts_skb_queue[i - c] = ptp->tx_ts_skb_queue[i];
		ptp->tx_ts_seconds_queue[i - c] = ptp->tx_ts_seconds_queue[i];
		ptp->tx_ts_nseconds_queue[i - c] = ptp->tx_ts_nseconds_queue[i];

		ptp->tx_ts_skb_queue[i] = NULL;
		ptp->tx_ts_seconds_queue[i] = 0;
		ptp->tx_ts_nseconds_queue[i] = 0;
	}
	ptp->tx_ts_skb_queue_size -= c;
	ptp->tx_ts_queue_size -= c;
done:
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     ptp->pending_tx_timestamps >= c);
	ptp->pending_tx_timestamps -= c;
	mutex_unlock(&ptp->tx_ts_lock);
}

static void lan743x_ptp_tx_ts_enqueue_skb(struct lan743x_adapter *adapter,
					  struct sk_buff *skb)
{
	struct lan743x_ptp *ptp = &adapter->ptp;

	NETIF_ASSERT(adapter, drv, adapter->netdev, skb);

	mutex_lock(&ptp->tx_ts_lock);
	if (ptp->tx_ts_skb_queue_size < LAN743X_PTP_NUMBER_OF_TX_TIMESTAMPS) {
		NETIF_ASSERT(adapter, drv, adapter->netdev,
			     !ptp->tx_ts_skb_queue[ptp->tx_ts_skb_queue_size]);
		ptp->tx_ts_skb_queue[ptp->tx_ts_skb_queue_size] = skb;
		ptp->tx_ts_skb_queue_size++;
	} else {
		/* this should never happen, so long as the tx channel
		 * calls and honors the result from
		 * lan743x_ptp_request_tx_timestamp
		 */
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "tx ts skb queue overflow");
		dev_kfree_skb(skb);
	}
	mutex_unlock(&ptp->tx_ts_lock);
}

static void lan743x_ptp_tx_ts_enqueue_ts(struct lan743x_adapter *adapter,
					 u32 seconds, u32 nano_seconds)
{
	struct lan743x_ptp *ptp = &adapter->ptp;

	mutex_lock(&ptp->tx_ts_lock);
	if (ptp->tx_ts_queue_size < LAN743X_PTP_NUMBER_OF_TX_TIMESTAMPS) {
		NETIF_ASSERT(adapter, drv, adapter->netdev,
			     !ptp->tx_ts_seconds_queue[
			     ptp->tx_ts_queue_size]);
		ptp->tx_ts_seconds_queue[ptp->tx_ts_queue_size] = seconds;
		NETIF_ASSERT(adapter, drv, adapter->netdev,
			     !ptp->tx_ts_nseconds_queue[
			     ptp->tx_ts_queue_size]);
		ptp->tx_ts_nseconds_queue[ptp->tx_ts_queue_size] = nano_seconds;
		ptp->tx_ts_queue_size++;
	} else {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "tx ts queue overflow");
	}
	mutex_unlock(&ptp->tx_ts_lock);
}

static void lan743x_ptp_isr_bottom_half(unsigned long param)
{
	struct lan743x_adapter *adapter = (struct lan743x_adapter *)param;
	bool new_timestamp_available = false;

	NETIF_ASSERT(adapter, drv, adapter->netdev, adapter);

	while (lan743x_csr_read(adapter, PTP_INT_STS) & PTP_INT_BIT_TX_TS_) {
		u32 cap_info = lan743x_csr_read(adapter, PTP_CAP_INFO);

		if (PTP_CAP_INFO_TX_TS_CNT_GET_(cap_info) > 0) {
			u32 seconds = lan743x_csr_read(adapter,
						       PTP_TX_EGRESS_SEC);
			u32 nsec = lan743x_csr_read(adapter, PTP_TX_EGRESS_NS);
			u32 cause = (nsec &
				    PTP_TX_EGRESS_NS_CAPTURE_CAUSE_MASK_);

			if (cause == PTP_TX_EGRESS_NS_CAPTURE_CAUSE_SW_) {
				nsec &= PTP_TX_EGRESS_NS_TS_NS_MASK_;
				lan743x_ptp_tx_ts_enqueue_ts(
					adapter, seconds, nsec);
				new_timestamp_available = true;
			} else if (cause ==
				   PTP_TX_EGRESS_NS_CAPTURE_CAUSE_AUTO_) {
				NETIF_ERROR(adapter, drv, adapter->netdev,
					    "Auto capture cause not supported");
			} else {
				NETIF_WARNING(adapter, drv, adapter->netdev,
					      "unknown tx timestamp capture cause");
			}
		} else {
			NETIF_WARNING(adapter, drv, adapter->netdev,
				      "TX TS INT but no TX TS CNT");
		}
		lan743x_csr_write(adapter, PTP_INT_STS, PTP_INT_BIT_TX_TS_);
	}

	if (new_timestamp_available)
		lan743x_ptp_tx_ts_complete(adapter);

	lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_1588_);
}

static void lan743x_ptp_sync_to_system_clock(struct lan743x_adapter *adapter)
{
	struct timeval tv;

	memset(&tv, 0, sizeof(tv));
	do_gettimeofday(&tv);
	lan743x_ptp_clock_set(adapter, tv.tv_sec, tv.tv_usec * 1000, 0);
}

static int lan743x_ptp_init(struct lan743x_adapter *adapter)
{
	struct lan743x_ptp *ptp = &adapter->ptp;

	NETIF_ASSERT(adapter, drv, adapter->netdev, ptp);
	memset(ptp, 0, sizeof(*ptp));

	mutex_init(&ptp->command_lock);
	mutex_init(&ptp->tx_ts_lock);

	tasklet_init(&ptp->ptp_isr_bottom_half,
		     lan743x_ptp_isr_bottom_half, (unsigned long)adapter);
	tasklet_disable(&ptp->ptp_isr_bottom_half);

	ptp->used_event_ch = 0;
	ptp->pps_event_ch = -1;
	ptp->pps_gpio_bit = -1;

	return 0;
}

static void lan743x_ptp_cleanup(struct lan743x_adapter *adapter)
{
	struct lan743x_ptp *ptp = &adapter->ptp;

	memset(ptp, 0, sizeof(*ptp));
}

static int lan743x_ptp_open(struct lan743x_adapter *adapter)
{
	struct lan743x_ptp *ptp = &adapter->ptp;
	int ret = -ENODEV;

	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     !ptp->pending_tx_timestamps);
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     !ptp->tx_ts_skb_queue_size);
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     !ptp->tx_ts_queue_size);

	lan743x_ptp_reset(adapter);
	lan743x_ptp_sync_to_system_clock(adapter);
	lan743x_ptp_enable(adapter);

	tasklet_enable(&ptp->ptp_isr_bottom_half);
	lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_1588_);
	lan743x_csr_write(adapter, PTP_INT_EN_SET,
			  PTP_INT_BIT_TX_SWTS_ERR_ | PTP_INT_BIT_TX_TS_);
	ptp->flags |= PTP_FLAG_ISR_ENABLED;

#ifdef CONFIG_PTP_1588_CLOCK
	snprintf(ptp->pin_config[0].name, 32, "lan743x_ptp_pin_0");
	ptp->pin_config[0].index = 0;
	ptp->pin_config[0].func = PTP_PF_PEROUT;
	ptp->pin_config[0].chan = 0;

	ptp->ptp_clock_info.owner = THIS_MODULE;
	snprintf(ptp->ptp_clock_info.name, 16, "%pm",
		 adapter->netdev->dev_addr);
	ptp->ptp_clock_info.max_adj = LAN743X_PTP_MAX_FREQ_ADJ_IN_PPB;
	ptp->ptp_clock_info.n_alarm = 0;
	ptp->ptp_clock_info.n_ext_ts = 0;
	ptp->ptp_clock_info.n_per_out = 0;
	ptp->ptp_clock_info.n_pins = 0;
	ptp->ptp_clock_info.pps = 1;
	ptp->ptp_clock_info.pin_config = NULL;
	ptp->ptp_clock_info.adjfreq = lan743x_ptpci_adjfreq;
	ptp->ptp_clock_info.adjtime = lan743x_ptpci_adjtime;
	ptp->ptp_clock_info.gettime64 = lan743x_ptpci_gettime64;
	ptp->ptp_clock_info.getcrosststamp = NULL;
	ptp->ptp_clock_info.settime64 = lan743x_ptpci_settime64;
	ptp->ptp_clock_info.enable = lan743x_ptpci_enable;
	ptp->ptp_clock_info.verify = NULL;

	ptp->ptp_clock = ptp_clock_register(&ptp->ptp_clock_info,
					    &adapter->pci.pdev->dev);

	if (IS_ERR(ptp->ptp_clock)) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "ptp_clock_register failed");
		goto done;
	}
	ptp->flags |= PTP_FLAG_PTP_CLOCK_REGISTERED;
	NETIF_INFO(adapter, ifup, adapter->netdev,
		   "successfully registered ptp clock");
#endif

	ret = 0;

#ifdef CONFIG_PTP_1588_CLOCK
done:
	if (ret)
		lan743x_ptp_close(adapter);
#endif

	return ret;
}

static void lan743x_ptp_close(struct lan743x_adapter *adapter)
{
	struct lan743x_ptp *ptp = &adapter->ptp;
	int index;

#ifdef CONFIG_PTP_1588_CLOCK
	if (ptp->flags & PTP_FLAG_PTP_CLOCK_REGISTERED) {
		NETIF_ASSERT(adapter, drv, adapter->netdev, ptp->ptp_clock);
		ptp_clock_unregister(ptp->ptp_clock);
		ptp->ptp_clock = NULL;
		ptp->flags &= ~PTP_FLAG_PTP_CLOCK_REGISTERED;
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "ptp clock unregister");
	}
#endif

	if (ptp->flags & PTP_FLAG_ISR_ENABLED) {
		lan743x_csr_write(adapter, PTP_INT_EN_CLR,
				  PTP_INT_BIT_TX_SWTS_ERR_ |
				  PTP_INT_BIT_TX_TS_);
		lan743x_csr_write(adapter, INT_EN_CLR, INT_BIT_1588_);
		tasklet_disable(&ptp->ptp_isr_bottom_half);
		ptp->flags &= ~PTP_FLAG_ISR_ENABLED;
	}

	/* clean up pending timestamp requests */
	lan743x_ptp_tx_ts_complete(adapter);
	mutex_lock(&ptp->tx_ts_lock);
	for (index = 0;
	     index < LAN743X_PTP_NUMBER_OF_TX_TIMESTAMPS;
	     index++) {
		struct sk_buff *skb = ptp->tx_ts_skb_queue[index];

		if (skb)
			dev_kfree_skb(skb);
		ptp->tx_ts_skb_queue[index] = NULL;
		ptp->tx_ts_seconds_queue[index] = 0;
		ptp->tx_ts_nseconds_queue[index] = 0;
	}
	ptp->tx_ts_skb_queue_size = 0;
	ptp->tx_ts_queue_size = 0;
	ptp->pending_tx_timestamps = 0;
	mutex_unlock(&ptp->tx_ts_lock);

	lan743x_ptp_disable(adapter);
}

static bool lan743x_ptp_is_enabled(struct lan743x_adapter *adapter)
{
	if (lan743x_csr_read(adapter, PTP_CMD_CTL) & PTP_CMD_CTL_PTP_ENABLE_)
		return true;
	return false;
}

static void lan743x_ptp_wait_till_cmd_done(struct lan743x_adapter *adapter,
					   u32 bit_mask)
{
	int timeout = 1000;
	u32 data = 0;

	while (timeout && (data = (lan743x_csr_read(
	       adapter, PTP_CMD_CTL) &
	       bit_mask))) {
		usleep_range(1000, 20000);
		timeout--;
	}
	if (data) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "timeout waiting for cmd to be done, cmd = 0x%08X",
			    bit_mask);
	}
}

static void lan743x_ptp_enable(struct lan743x_adapter *adapter)
{
	struct lan743x_ptp *ptp = &adapter->ptp;

	mutex_lock(&ptp->command_lock);

	if (lan743x_ptp_is_enabled(adapter)) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "PTP already enabled");
		goto done;
	}
	lan743x_csr_write(adapter, PTP_CMD_CTL, PTP_CMD_CTL_PTP_ENABLE_);
done:
	mutex_unlock(&ptp->command_lock);
}

static void lan743x_ptp_disable(struct lan743x_adapter *adapter)
{
	struct lan743x_ptp *ptp = &adapter->ptp;

	mutex_lock(&ptp->command_lock);
	if (!lan743x_ptp_is_enabled(adapter)) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "PTP already disabled");
		goto done;
	}

	lan743x_csr_write(adapter, PTP_CMD_CTL,	PTP_CMD_CTL_PTP_DISABLE_);
	lan743x_ptp_wait_till_cmd_done(adapter, PTP_CMD_CTL_PTP_ENABLE_);
done:
	mutex_unlock(&ptp->command_lock);
}

static void lan743x_ptp_reset(struct lan743x_adapter *adapter)
{
	struct lan743x_ptp *ptp = &adapter->ptp;

	mutex_lock(&ptp->command_lock);

	if (lan743x_ptp_is_enabled(adapter)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Attempting reset while enabled");
		goto done;
	}

	lan743x_csr_write(adapter, PTP_CMD_CTL,	PTP_CMD_CTL_PTP_RESET_);
	lan743x_ptp_wait_till_cmd_done(adapter, PTP_CMD_CTL_PTP_RESET_);
done:
	mutex_unlock(&ptp->command_lock);
}

#ifdef CONFIG_PTP_1588_CLOCK
static int lan743x_ptp_reserve_event_ch(struct lan743x_adapter *adapter)
{
	struct lan743x_ptp *ptp = &adapter->ptp;
	int index = 0;
	int result = -ENODEV;

	mutex_lock(&ptp->command_lock);
	for (index = 0; index < LAN743X_PTP_NUMBER_OF_EVENT_CHANNELS; index++) {
		if (!(test_bit(index, &ptp->used_event_ch))) {
			ptp->used_event_ch |= BIT(index);
			result = index;
			break;
		}
	}
	mutex_unlock(&ptp->command_lock);
	return result;
}
#endif /* CONFIG_PTP_1588_CLOCK */

#ifdef CONFIG_PTP_1588_CLOCK
static void lan743x_ptp_release_event_ch(struct lan743x_adapter *adapter,
					 int event_channel)
{
	struct lan743x_ptp *ptp = &adapter->ptp;

	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     (event_channel >= 0) &&
		     (event_channel < LAN743X_PTP_NUMBER_OF_EVENT_CHANNELS));
	mutex_lock(&ptp->command_lock);
	if (test_bit(event_channel, &ptp->used_event_ch)) {
		ptp->used_event_ch &= ~BIT(event_channel);
	} else {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "attempted release on a not used event_channel = %d",
			      event_channel);
	}
	mutex_unlock(&ptp->command_lock);
}
#endif /* CONFIG_PTP_1588_CLOCK */

#ifdef CONFIG_PTP_1588_CLOCK
static void lan743x_ptp_clock_get(struct lan743x_adapter *adapter,
				  u32 *seconds, u32 *nano_seconds,
				  u32 *sub_nano_seconds)
{
	struct lan743x_ptp *ptp = &adapter->ptp;

	mutex_lock(&ptp->command_lock);

	lan743x_csr_write(adapter, PTP_CMD_CTL, PTP_CMD_CTL_PTP_CLOCK_READ_);
	lan743x_ptp_wait_till_cmd_done(adapter, PTP_CMD_CTL_PTP_CLOCK_READ_);

	if (seconds)
		(*seconds) = lan743x_csr_read(adapter, PTP_CLOCK_SEC);

	if (nano_seconds)
		(*nano_seconds) = lan743x_csr_read(adapter, PTP_CLOCK_NS);

	if (sub_nano_seconds)
		(*sub_nano_seconds) =
			lan743x_csr_read(adapter, PTP_CLOCK_SUBNS);

	mutex_unlock(&ptp->command_lock);
}
#endif /* CONFIG_PTP_1588_CLOCK */

static void lan743x_ptp_clock_set(struct lan743x_adapter *adapter,
				  u32 seconds, u32 nano_seconds,
				  u32 sub_nano_seconds)
{
	struct lan743x_ptp *ptp = &adapter->ptp;

	mutex_lock(&ptp->command_lock);

	lan743x_csr_write(adapter, PTP_CLOCK_SEC, seconds);
	lan743x_csr_write(adapter, PTP_CLOCK_NS, nano_seconds);
	lan743x_csr_write(adapter, PTP_CLOCK_SUBNS, sub_nano_seconds);

	lan743x_csr_write(adapter, PTP_CMD_CTL, PTP_CMD_CTL_PTP_CLOCK_LOAD_);
	lan743x_ptp_wait_till_cmd_done(adapter, PTP_CMD_CTL_PTP_CLOCK_LOAD_);
	mutex_unlock(&ptp->command_lock);
}

#ifdef CONFIG_PTP_1588_CLOCK
static void lan743x_ptp_clock_step(struct lan743x_adapter *adapter,
				   s64 time_step_ns)
{
	struct lan743x_ptp *ptp = &adapter->ptp;
	u64 abs_time_step_ns = 0;
	s32 seconds = 0;
	u32 nano_seconds = 0;

	if (time_step_ns >  15000000000LL) {
		/* convert to clock set */
		u32 seconds = 0;
		u32 nano_seconds = 0;

		lan743x_ptp_clock_get(adapter, &seconds, &nano_seconds, NULL);
		seconds += (time_step_ns / 1000000000LL);
		nano_seconds += (time_step_ns % 1000000000LL);
		if (nano_seconds >= 1000000000) {
			seconds++;
			nano_seconds -= 1000000000;
		}
		lan743x_ptp_clock_set(adapter, seconds, nano_seconds, 0);
		return;
	} else if (time_step_ns < -15000000000LL) {
		/* convert to clock set */
		u32 seconds = 0;
		u32 nano_seconds = 0;
		u32 nano_seconds_step = 0;

		lan743x_ptp_clock_get(adapter, &seconds, &nano_seconds, NULL);
		seconds -= (time_step_ns / 1000000000LL);
		nano_seconds_step = (time_step_ns % 1000000000LL);
		if (nano_seconds < nano_seconds_step) {
			seconds--;
			nano_seconds += 1000000000;
		}
		nano_seconds -= nano_seconds_step;
		lan743x_ptp_clock_set(adapter, seconds, nano_seconds, 0);
		return;
	}

	/* do clock step */

	if (time_step_ns >= 0) {
		abs_time_step_ns = (u64)(time_step_ns);
		seconds = (s32)(abs_time_step_ns / 1000000000);
		nano_seconds = (u32)(abs_time_step_ns % 1000000000);
	} else {
		abs_time_step_ns = (u64)(-time_step_ns);
		seconds = -((s32)(abs_time_step_ns / 1000000000));
		nano_seconds = (u32)(abs_time_step_ns % 1000000000);
		if (nano_seconds > 0) {
			/* subtracting nano seconds is not allowed
			 * convert to subtracting from seconds,
			 * and adding to nanoseconds
			 */
			seconds--;
			nano_seconds = (1000000000 - nano_seconds);
		}
	}

	if (nano_seconds > 0) {
		/* add 8 ns to cover the likely normal increment */
		nano_seconds += 8;
	}

	if (nano_seconds >= 1000000000) {
		/* carry into seconds */
		seconds++;
		nano_seconds -= 1000000000;
	}

	while (seconds) {
		mutex_lock(&ptp->command_lock);
		if (seconds > 0) {
			u32 adjustment_value = (u32)seconds;

			if (adjustment_value > 0xF)
				adjustment_value = 0xF;
			lan743x_csr_write(adapter, PTP_CLOCK_STEP_ADJ,
					  PTP_CLOCK_STEP_ADJ_DIR_ |
					  adjustment_value);
			seconds -= ((s32)adjustment_value);
		} else {
			u32 adjustment_value = (u32)(-seconds);

			if (adjustment_value > 0xF)
				adjustment_value = 0xF;
			lan743x_csr_write(adapter, PTP_CLOCK_STEP_ADJ,
					  adjustment_value);
			seconds += ((s32)adjustment_value);
		}
		lan743x_csr_write(adapter, PTP_CMD_CTL,
				  PTP_CMD_CTL_PTP_CLOCK_STEP_SEC_);
		lan743x_ptp_wait_till_cmd_done(adapter,
					       PTP_CMD_CTL_PTP_CLOCK_STEP_SEC_);
		mutex_unlock(&ptp->command_lock);
	}
	if (nano_seconds) {
		NETIF_ASSERT(adapter, drv, adapter->netdev,
			     nano_seconds < 1000000000);
		mutex_lock(&ptp->command_lock);
		lan743x_csr_write(adapter, PTP_CLOCK_STEP_ADJ,
				  PTP_CLOCK_STEP_ADJ_DIR_ |
				  (nano_seconds &
				  PTP_CLOCK_STEP_ADJ_VALUE_MASK_));
		lan743x_csr_write(adapter, PTP_CMD_CTL,
				  PTP_CMD_CTL_PTP_CLK_STP_NSEC_);
		lan743x_ptp_wait_till_cmd_done(adapter,
					       PTP_CMD_CTL_PTP_CLK_STP_NSEC_);
		mutex_unlock(&ptp->command_lock);
	}
}
#endif /* CONFIG_PTP_1588_CLOCK */

static bool lan743x_ptp_request_tx_timestamp(struct lan743x_adapter *adapter)
{
	struct lan743x_ptp *ptp = &adapter->ptp;
	bool result = false;

	mutex_lock(&ptp->tx_ts_lock);
	if (ptp->pending_tx_timestamps < LAN743X_PTP_NUMBER_OF_TX_TIMESTAMPS) {
		ptp->pending_tx_timestamps++;
		result = true;/* request granted */
	}
	mutex_unlock(&ptp->tx_ts_lock);
	return result;
}

static void lan743x_ptp_unrequest_tx_timestamp(struct lan743x_adapter *adapter)
{
	struct lan743x_ptp *ptp = &adapter->ptp;

	mutex_lock(&ptp->tx_ts_lock);
	if (ptp->pending_tx_timestamps > 0) {
		ptp->pending_tx_timestamps--;
	} else {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "unrequest failed, pending_tx_timestamps==0");
	}
	mutex_unlock(&ptp->tx_ts_lock);
}

static void lan743x_ptp_tx_timestamp_skb(struct lan743x_adapter *adapter,
					 struct sk_buff *skb)
{
	NETIF_ASSERT(adapter, drv, adapter->netdev, skb);
	lan743x_ptp_tx_ts_enqueue_skb(adapter, skb);

	lan743x_ptp_tx_ts_complete(adapter);
}

#ifdef CONFIG_PTP_1588_CLOCK
static int lan743x_ptp_get_clock_index(struct lan743x_adapter *adapter)
{
	struct lan743x_ptp *ptp = &adapter->ptp;

	if (ptp->ptp_clock)
		return ptp_clock_index(ptp->ptp_clock);
	return -1;
}
#endif /* CONFIG_PTP_1588_CLOCK */

/* MAC */
#define MAC_FLAG_MDIOBUS_ALLOCATED      BIT(0)
#define MAC_FLAG_MDIOBUS_REGISTERED     BIT(1)

static void lan743x_mac_isr(void *context)
{
	struct lan743x_adapter *adapter = (struct lan743x_adapter *)context;
	struct lan743x_mac *mac = NULL;
	u32 mac_int_sts = 0;
	u32 mac_int_en = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, adapter);
	mac = &adapter->mac;

	/* disable isr */
	lan743x_csr_write(adapter, INT_EN_CLR, INT_BIT_MAC_);

	mac_int_sts = lan743x_csr_read(adapter, MAC_INT_STS);
	mac_int_en = lan743x_csr_read(adapter, MAC_INT_EN_SET);
	mac_int_sts = mac_int_sts & mac_int_en;
	if (mac_int_sts & MAC_INT_BIT_MAC_ERR_) {
		u32 err_sts = lan743x_csr_read(adapter, MAC_ERR_STS);

		if (err_sts & MAC_ERR_STS_RESERVED_)
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Reserved ERROR, err_sts = 0x%08X",
				    err_sts);
		if (err_sts & MAC_ERR_STS_LEN_ERR_)
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Length Field Error");
		if (err_sts & MAC_ERR_STS_RXERR_)
			NETIF_ERROR(adapter, drv, adapter->netdev, "RX Error");
		if (err_sts & MAC_ERR_STS_LFERR_)
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Large Frame Error");
		if (err_sts & MAC_ERR_STS_RWTERR_)
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Receive Watchdog Timer Expired");
		if (err_sts & MAC_ERR_STS_ECERR_)
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Excessive Collision Error");
		if (err_sts & MAC_ERR_STS_URERR_)
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Under Run Error");

		/* clear error bits */
		lan743x_csr_write(adapter, MAC_ERR_STS, err_sts);
	}
	if (mac_int_sts & (~MAC_INT_BIT_MAC_ERR_))
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Unhandled MAC Interrupt");

	/* clear mac int status bits */
	lan743x_csr_write(adapter, MAC_INT_STS, mac_int_sts);

	/* enable isr */
	lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_MAC_);
}

static int lan743x_mdiobus_read(struct mii_bus *bus, int phy_id, int index);
static int lan743x_mdiobus_write(struct mii_bus *bus, int phy_id, int index,
				 u16 regval);

static int lan743x_mac_reset(struct lan743x_adapter *adapter)
{
	u32 data = 0;
	int timeout = 100;

	lan743x_csr_write(adapter, MAC_CR, MAC_CR_RST_);
	while (timeout &&
	       ((data = lan743x_csr_read(adapter, MAC_CR)) & MAC_CR_RST_)) {
		usleep_range(1000, 20000);
		timeout--;
	}
	if (data & MAC_CR_RST_) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "timed out waiting for mac reset to finish");
		return -EIO;
	}
	return 0;
}

static int lan743x_mac_init(struct lan743x_adapter *adapter)
{
	struct lan743x_mac *mac = &adapter->mac;
	struct net_device *netdev;
	u32 data;
	int ret = -ENODEV;
	u32 mac_addr_hi = 0;
	u32 mac_addr_lo = 0;
	bool mac_address_valid = true;

	NETIF_ASSERT(adapter, probe, adapter->netdev, mac);

	memset(mac, 0, sizeof(*mac));

	netdev = adapter->netdev;

	ret = lan743x_mac_reset(adapter);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "mac reset failed");
		goto clean_up;
	}

	/* setup auto duplex, and speed detection */
	data = lan743x_csr_read(adapter, MAC_CR);
	data |= MAC_CR_ADD_ | MAC_CR_ASD_;
	data |= MAC_CR_CNTR_RST_;
	lan743x_csr_write(adapter, MAC_CR, data);

	mutex_init(&mac->tx_mutex);
	mac->tx_enable_bits = 0;
	mutex_init(&mac->rx_mutex);
	mac->rx_enable_bits = 0;

	mac->mdiobus = mdiobus_alloc();
	if (!(mac->mdiobus)) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "mdiobus_alloc failed");
		ret = -ENOMEM;
		goto clean_up;
	}
	mac->flags |= MAC_FLAG_MDIOBUS_ALLOCATED;

	mutex_init(&mac->mii_mutex);
	mac->mdiobus->priv = (void *)adapter;
	mac->mdiobus->read = lan743x_mdiobus_read;
	mac->mdiobus->write = lan743x_mdiobus_write;
	mac->mdiobus->name = "lan743x-mdiobus";

	snprintf(mac->mdiobus->id, MII_BUS_ID_SIZE,
		 "pci-%s", pci_name(adapter->pci.pdev));

	/* set to internal PHY id */
	mac->mdiobus->phy_mask = ~(1 << 1);

	/* register mdiobus */
	ret = mdiobus_register(mac->mdiobus);
	if (ret < 0) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "failed to register MDIO bus");
		goto clean_up;
	}
	NETIF_INFO(adapter, probe, adapter->netdev,
		   "successfully registered MDIO bus, %s", mac->mdiobus->id);
	mac->flags |= MAC_FLAG_MDIOBUS_REGISTERED;

	mac_addr_hi = lan743x_csr_read(adapter, MAC_RX_ADDRH);
	mac_addr_lo = lan743x_csr_read(adapter, MAC_RX_ADDRL);
	mac->mac_address[0] = mac_addr_lo & 0xFF;
	mac->mac_address[1] = (mac_addr_lo >> 8) & 0xFF;
	mac->mac_address[2] = (mac_addr_lo >> 16) & 0xFF;
	mac->mac_address[3] = (mac_addr_lo >> 24) & 0xFF;
	mac->mac_address[4] = mac_addr_hi & 0xFF;
	mac->mac_address[5] = (mac_addr_hi >> 8) & 0xFF;

	if (((mac_addr_hi & 0x0000FFFF) == 0x0000FFFF) &&
	    (mac_addr_lo == 0xFFFFFFFF)) {
		NETIF_INFO(adapter, probe, adapter->netdev,
			   "MAC address not available from EEPROM or OTP");
		mac_address_valid = false;
	} else if (!is_valid_ether_addr(mac->mac_address)) {
		NETIF_WARNING(adapter, probe, adapter->netdev,
			      "MAC address is not valid");
		mac_address_valid = false;
	}

	if (!mac_address_valid) {
		random_ether_addr(mac->mac_address);
		NETIF_INFO(adapter, probe, adapter->netdev,
			   "MAC address set to random address");
		mac_addr_lo = mac->mac_address[0] |
			      (mac->mac_address[1] << 8) |
			      (mac->mac_address[2] << 16) |
			      (mac->mac_address[3] << 24);
		mac_addr_hi = mac->mac_address[4] |
			      (mac->mac_address[5] << 8);
	}

	lan743x_csr_write(adapter, MAC_RX_ADDRL, mac_addr_lo);
	lan743x_csr_write(adapter, MAC_RX_ADDRH, mac_addr_hi);
	NETIF_INFO(adapter, probe, adapter->netdev,
		   "MAC Address = %02X:%02X:%02X:%02X:%02X:%02X",
		   mac->mac_address[0], mac->mac_address[1],
		   mac->mac_address[2], mac->mac_address[3],
		   mac->mac_address[4], mac->mac_address[5]);

	ether_addr_copy(netdev->dev_addr, mac->mac_address);

	ret = 0;

clean_up:
	if (ret)
		lan743x_mac_cleanup(adapter);
	return ret;
}

static void lan743x_mac_cleanup(struct lan743x_adapter *adapter)
{
	struct lan743x_mac *mac = &adapter->mac;

	if (mac->tx_enable_bits) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Some TX channels have not been disabled");
	}
	if (mac->rx_enable_bits) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Some RX Channels have not been disabled");
	}

	if (mac->flags & MAC_FLAG_MDIOBUS_REGISTERED) {
		mdiobus_unregister(mac->mdiobus);
		mac->flags &= ~MAC_FLAG_MDIOBUS_REGISTERED;
	}

	if (mac->flags & MAC_FLAG_MDIOBUS_ALLOCATED) {
		NETIF_ASSERT(adapter, drv, adapter->netdev, mac->mdiobus);
		mdiobus_free(mac->mdiobus);
		mac->mdiobus = NULL;
		mac->flags &= ~MAC_FLAG_MDIOBUS_ALLOCATED;
	}

	memset(mac, 0, sizeof(*mac));
}

static int lan743x_mac_open(struct lan743x_adapter *adapter)
{
	lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_MAC_);
	lan743x_csr_write(adapter, MAC_INT_EN_SET, MAC_INT_BIT_MAC_ERR_);

	return 0;
}

static void lan743x_mac_close(struct lan743x_adapter *adapter)
{
	lan743x_csr_write(adapter, MAC_INT_EN_CLR, MAC_INT_BIT_MAC_ERR_);
	lan743x_csr_write(adapter, INT_EN_CLR, INT_BIT_MAC_);
}

static void lan743x_mac_get_address(struct lan743x_adapter *adapter,
				    u8 *mac_addr)
{
	struct lan743x_mac *mac = &adapter->mac;

	NETIF_ASSERT(adapter, drv, adapter->netdev, mac_addr);
	ether_addr_copy(mac_addr, mac->mac_address);
}

#define MAC_MII_READ            1
#define MAC_MII_WRITE           0
static inline u32 lan743x_mac_mii_access(int id, int index, int read)
{
	u32 ret;

	ret = ((u32)id << MAC_MII_ACC_PHY_ADDR_SHIFT_) &
	      MAC_MII_ACC_PHY_ADDR_MASK_;
	ret |= ((u32)index << MAC_MII_ACC_MIIRINDA_SHIFT_) &
	       MAC_MII_ACC_MIIRINDA_MASK_;
	if (read)
		ret |= MAC_MII_ACC_MII_READ_;
	else
		ret |= MAC_MII_ACC_MII_WRITE_;
	ret |= MAC_MII_ACC_MII_BUSY_;

	return ret;
}

static int lan743x_mac_mii_wait_till_not_busy(struct lan743x_adapter *adapter)
{
	unsigned long start_time = jiffies;
	u32 data;

	do {
		data = lan743x_csr_read(adapter, MAC_MII_ACC);

		if (!(data & MAC_MII_ACC_MII_BUSY_))
			return 0;
	} while (!time_after(jiffies, start_time + HZ));

	NETIF_ERROR(adapter, drv, adapter->netdev, "mii is busy");
	return -EIO;
}

static int lan743x_mac_mii_read(struct lan743x_adapter *adapter,
				int phy_id, int index)
{
	struct lan743x_mac *mac = &adapter->mac;
	u32 val, addr;
	int ret;

	mutex_lock(&mac->mii_mutex);

	/* comfirm MII not busy */
	ret = lan743x_mac_mii_wait_till_not_busy(adapter);
	if (ret < 0)
		goto done;

	/* set the address, index & direction (read from PHY) */
	addr = lan743x_mac_mii_access(phy_id, index, MAC_MII_READ);
	lan743x_csr_write(adapter, MAC_MII_ACC, addr);

	ret = lan743x_mac_mii_wait_till_not_busy(adapter);
	if (ret < 0)
		goto done;

	val = lan743x_csr_read(adapter, MAC_MII_DATA);

	ret = (int)(val & 0xFFFF);

done:
	mutex_unlock(&mac->mii_mutex);
#if (LAN743X_PHY_TRACE_ENABLE != 0)
	NETIF_INFO(adapter, drv, adapter->netdev,
		   "MII READ: phy_id = %d, index = %d, value = 0x%04X",
		   phy_id, index, ret);
#endif
	return ret;
}

static int lan743x_mdiobus_read(struct mii_bus *bus, int phy_id, int index)
{
	struct lan743x_adapter *adapter = bus->priv;

	return lan743x_mac_mii_read(adapter, phy_id, index);
}

static int lan743x_mac_mii_write(struct lan743x_adapter *adapter,
				 int phy_id, int index, u16 regval)
{
	struct lan743x_mac *mac = &adapter->mac;
	u32 val, addr;
	int ret;

#if (LAN743X_PHY_TRACE_ENABLE != 0)
	NETIF_INFO(adapter, drv, adapter->netdev,
		   "MII WRITE: phy_id = %d, index = %d, value = 0x%04X",
		   phy_id, index, regval);
#endif

	mutex_lock(&mac->mii_mutex);

	/* confirm MII not busy */
	ret = lan743x_mac_mii_wait_till_not_busy(adapter);
	if (ret < 0)
		goto done;

	val = (u32)regval;
	lan743x_csr_write(adapter, MAC_MII_DATA, val);

	/* set the address, index & direction (write to PHY) */
	addr = lan743x_mac_mii_access(phy_id, index, MAC_MII_WRITE);
	lan743x_csr_write(adapter, MAC_MII_ACC, addr);

	ret = lan743x_mac_mii_wait_till_not_busy(adapter);

done:
	mutex_unlock(&mac->mii_mutex);
	return ret;
}

static int lan743x_mdiobus_write(struct mii_bus *bus,
				 int phy_id, int index, u16 regval)
{
	struct lan743x_adapter *adapter = bus->priv;

	return lan743x_mac_mii_write(adapter, phy_id, index, regval);
}

static void lan743x_mac_flow_ctrl_set_enables(struct lan743x_adapter *adapter,
					      bool tx_enable, bool rx_enable)
{
	u32 flow_setting = 0;

	/* set maximum pause time because when fifo space frees
	 * up a zero value pause frame will be sent to release the pause
	 */
	flow_setting = MAC_FLOW_CR_FCPT_MASK_;

	if (tx_enable)
		flow_setting |= MAC_FLOW_CR_TX_FCEN_;

	if (rx_enable)
		flow_setting |= MAC_FLOW_CR_RX_FCEN_;

	lan743x_csr_write(adapter, MAC_FLOW, flow_setting);
}

static int lan743x_mac_tx_enable_all(struct lan743x_adapter *adapter)
{
	u32 data = 0;

	data = lan743x_csr_read(adapter, MAC_TX);
	if (data & MAC_TX_TXEN_) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Attempted to enable mac tx, when already enabled");
		goto done;
	}

	lan743x_csr_write(adapter, MAC_TX, data | MAC_TX_TXEN_);

done:
	return 0;
}

static int lan743x_mac_tx_disable_all(struct lan743x_adapter *adapter)
{
	int ret = 0;
	u32 data = 0;
	int timeout = 100;

	data = lan743x_csr_read(adapter, MAC_TX);
	if (!(data & MAC_TX_TXEN_)) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Attempted to disable mac tx, when already disabled");
		ret = 0;
		goto done;
	}
	if (data & MAC_TX_TXD_) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "TXD unexpectedly set, clearing now");
		lan743x_csr_write(adapter, MAC_TX, data);
		data &= ~MAC_TX_TXD_;
	}
	data &= ~MAC_TX_TXEN_;
	lan743x_csr_write(adapter, MAC_TX, data);
	while (timeout &&
	       (!((data = lan743x_csr_read(adapter, MAC_TX)) & MAC_TX_TXD_))) {
		usleep_range(1000, 20000);
		timeout--;
	}
	if (!(data & MAC_TX_TXD_)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "timed out waiting for mac to disable tx");
		ret = -EIO;
	} else {
		/* clear TXD */
		lan743x_csr_write(adapter, MAC_TX, data);
	}
	ret = 0;
done:
	return ret;
}

static int lan743x_mac_tx_enable(struct lan743x_adapter *adapter,
				 int tx_channel)
{
	struct lan743x_mac *mac = &adapter->mac;
	int ret = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (tx_channel >= 0) &&
		     (tx_channel < LAN743X_NUMBER_OF_TX_CHANNELS));
	mutex_lock(&mac->tx_mutex);
	if (test_bit(tx_channel, &mac->tx_enable_bits)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "attempting to enable an already enabled tx channel = %d",
			    tx_channel);
		goto done;
	}
	if (!mac->tx_enable_bits) {
		ret = lan743x_mac_tx_enable_all(adapter);
		if (ret) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Failed to enable mac");
			goto done;
		}
	}
	mac->tx_enable_bits |= BIT(tx_channel);
	ret = 0;
done:
	mutex_unlock(&mac->tx_mutex);
	return ret;
}

static int lan743x_mac_tx_disable(struct lan743x_adapter *adapter,
				  int tx_channel)
{
	struct lan743x_mac *mac = &adapter->mac;
	int ret = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (tx_channel >= 0) &&
		     (tx_channel < LAN743X_NUMBER_OF_TX_CHANNELS));
	mutex_lock(&mac->tx_mutex);
	if (!(test_bit(tx_channel, &mac->tx_enable_bits))) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "attempting to disable an already disabled tx channel = %d",
			    tx_channel);
		goto done;
	}
	mac->tx_enable_bits &= ~BIT(tx_channel);
	if (!mac->tx_enable_bits) {
		ret = lan743x_mac_tx_disable_all(adapter);
		if (ret) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Failed to disable mac");
			goto done;
		}
	}
	ret = 0;
done:
	mutex_unlock(&mac->tx_mutex);
	return ret;
}

static int lan743x_mac_rx_enable_all(struct lan743x_adapter *adapter)
{
	u32 data = 0;

	data = lan743x_csr_read(adapter, MAC_RX);
	if (data & MAC_RX_RXEN_) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Attempted to enable mac rx, when already enabled");
		goto done;
	}

	lan743x_csr_write(adapter, MAC_RX, data | MAC_RX_RXEN_);

done:
	return 0;
}

static int lan743x_mac_rx_disable_all(struct lan743x_adapter *adapter)
{
	int ret = 0;
	u32 data = 0;
	int timeout = 100;

	data = lan743x_csr_read(adapter, MAC_RX);
	if (!(data & MAC_RX_RXEN_)) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Attempted to disable mac rx, when already disabled");
		ret = 0;
		goto done;
	}
	if (data & MAC_RX_RXD_) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "RXD unexpectedly set, clearing now");
		lan743x_csr_write(adapter, MAC_RX, data);
		data &= ~MAC_RX_RXD_;
	}
	data &= ~MAC_RX_RXEN_;
	lan743x_csr_write(adapter, MAC_RX, data);
	while (timeout &&
	       (!((data = lan743x_csr_read(adapter, MAC_RX)) & MAC_RX_RXD_))) {
		usleep_range(1000, 20000);
		timeout--;
	}
	if (!(data & MAC_RX_RXD_)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "timed out waiting for mac to disable rx");
		ret = -EIO;
	} else {
		/* clear RXD */
		lan743x_csr_write(adapter, MAC_RX, data);
	}
	ret = 0;
done:
	return ret;
}

static int lan743x_mac_rx_enable(struct lan743x_adapter *adapter,
				 int rx_channel)
{
	struct lan743x_mac *mac = &adapter->mac;
	int ret = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (rx_channel >= 0) &&
		     (rx_channel < LAN743X_NUMBER_OF_RX_CHANNELS));
	mutex_lock(&mac->rx_mutex);
	if (test_bit(rx_channel, &mac->rx_enable_bits)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "attempting to enable an already enabled rx channel = %d",
			    rx_channel);
		goto done;
	}
	if (!mac->rx_enable_bits) {
		ret = lan743x_mac_rx_enable_all(adapter);
		if (ret) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Failed to enable mac");
			goto done;
		}
	}
	mac->rx_enable_bits |= BIT(rx_channel);
	ret = 0;
done:
	mutex_unlock(&mac->rx_mutex);
	return ret;
}

static int lan743x_mac_rx_disable(struct lan743x_adapter *adapter,
				  int rx_channel)
{
	struct lan743x_mac *mac = &adapter->mac;
	int ret = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (rx_channel >= 0) &&
		     (rx_channel < LAN743X_NUMBER_OF_RX_CHANNELS));
	mutex_lock(&mac->rx_mutex);
	if (!(test_bit(rx_channel, &mac->rx_enable_bits))) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "attempting to disable an already disabled rx channel = %d",
			    rx_channel);
		goto done;
	}
	mac->rx_enable_bits &= ~BIT(rx_channel);
	if (!mac->rx_enable_bits) {
		ret = lan743x_mac_rx_disable_all(adapter);
		if (ret) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Failed to disable mac");
			goto done;
		}
	}
	ret = 0;
done:
	mutex_unlock(&mac->rx_mutex);
	return ret;
}

static int lan743x_mac_set_mtu(struct lan743x_adapter *adapter, int new_mtu)
{
	struct lan743x_mac *mac = &adapter->mac;
	int ret = 0;
	u32 mac_rx = 0;

	if (new_mtu > LAN743X_MAX_FRAME_SIZE)
		return -EINVAL;
	if (new_mtu <= 0)
		return -EINVAL;

	mutex_lock(&mac->rx_mutex);
	if (mac->rx_enable_bits) {
		ret = lan743x_mac_rx_disable_all(adapter);
		if (ret) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Failed to disable mac");
			goto done;
		}
	}

	mac_rx = lan743x_csr_read(adapter, MAC_RX);
	mac_rx &= ~(MAC_RX_MAX_SIZE_MASK_);
	mac_rx |= (((new_mtu + ETH_HLEN + 4) << MAC_RX_MAX_SIZE_SHIFT_) &
		  MAC_RX_MAX_SIZE_MASK_);
	lan743x_csr_write(adapter, MAC_RX, mac_rx);

	if (mac->rx_enable_bits) {
		ret = lan743x_mac_rx_enable_all(adapter);
		if (ret) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Failed to enable mac");
			goto done;
		}
	}
done:
	mutex_unlock(&mac->rx_mutex);
	return ret;
}

static struct net_device_stats *mac_get_stats(struct lan743x_adapter *adapter)
{
	struct lan743x_mac *mac = &adapter->mac;

	memset(&mac->statistics, 0, sizeof(mac->statistics));
	mac->statistics.rx_packets = lan743x_csr_read(adapter,
						      STAT_RX_TOTAL_FRAMES);
	mac->statistics.tx_packets = lan743x_csr_read(adapter,
						      STAT_TX_TOTAL_FRAMES);
	return &mac->statistics;
}

static void lan743x_mac_set_address(struct lan743x_adapter *adapter,
				    u8 *addr)
{
	u32 addr_lo, addr_hi;

	addr_lo = addr[0] |
		  addr[1] << 8 |
		  addr[2] << 16 |
		  addr[3] << 24;
	addr_hi = addr[4] |
		  addr[5] << 8;

	lan743x_csr_write(adapter, MAC_RX_ADDRL, addr_lo);
	lan743x_csr_write(adapter, MAC_RX_ADDRH, addr_hi);

	ether_addr_copy(adapter->mac.mac_address, addr);

	NETIF_INFO(adapter, drv, adapter->netdev,
		   "MAC address set to %02X:%02X:%02X:%02X:%02X:%02X",
		   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

/* PHY */

#define PHY_FLAG_OPENED     BIT(0)
#define PHY_FLAG_ATTACHED   BIT(1)

static int lan743x_phy_reset(struct lan743x_adapter *adapter)
{
	int ret = -EIO;
	u32 data;
	unsigned long timeout;

	data = lan743x_csr_read(adapter, PMT_CTL);
	data |= PMT_CTL_ETH_PHY_RST_;
	lan743x_csr_write(adapter, PMT_CTL, data);

	timeout = jiffies + HZ;

	do {
		if (time_after(jiffies, timeout)) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "timeout, incomplete phy reset");
			ret = -EIO;
			goto done;
		}
		msleep(50);
		data = lan743x_csr_read(adapter, PMT_CTL);
	} while ((data & PMT_CTL_ETH_PHY_RST_) || !(data & PMT_CTL_READY_));

	ret = 0;
done:
	return ret;
}

static void lan743x_phy_update_flowcontrol(struct lan743x_adapter *adapter,
					   u8 duplex, u16 local_adv,
					   u16 remote_adv)
{
	struct lan743x_phy *phy = &adapter->phy;
	u8 cap;

	if (phy->fc_autoneg)
		cap = mii_resolve_flowctrl_fdx(local_adv, remote_adv);
	else
		cap = phy->fc_request_control;

	lan743x_mac_flow_ctrl_set_enables(adapter,
					  cap & FLOW_CTRL_TX,
					  cap & FLOW_CTRL_RX);

	NETIF_INFO(adapter, drv, adapter->netdev, "rx pause %s, tx pause %s",
		   (cap & FLOW_CTRL_RX ? "enabled" : "disabled"),
		   (cap & FLOW_CTRL_TX ? "enabled" : "disabled"));
}

static int lan743x_phy_init(struct lan743x_adapter *adapter)
{
	struct lan743x_phy *phy = &adapter->phy;
	int ret = -ENODEV;
	struct net_device *netdev;

	NETIF_ASSERT(adapter, probe, adapter->netdev, phy);

	netdev = adapter->netdev;

	memset(phy, 0, sizeof(struct lan743x_phy));

	ret = lan743x_phy_reset(adapter);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "phy reset failed, ret = %d", ret);
		goto clean_up;
	}

	/* carrier off reporting is important to ethtool even BEFORE open */
	netif_carrier_off(netdev);

	ret = 0;

clean_up:
	if (ret)
		lan743x_phy_cleanup(adapter);
	return ret;
}

static void lan743x_phy_cleanup(struct lan743x_adapter *adapter)
{
	struct lan743x_phy *phy = &adapter->phy;

	memset(phy, 0, sizeof(struct lan743x_phy));
}

static void lan743x_phy_link_status_change(struct net_device *netdev)
{
	struct lan743x_adapter *adapter = netdev_priv(netdev);
	struct phy_device *phydev = netdev->phydev;

	if (phydev) {
		if (phydev->state == PHY_RUNNING) {
			struct ethtool_link_ksettings ksettings;
			int local_advertisement = 0;
			int remote_advertisement = 0;
			struct lan743x_phy *phy = NULL;

			NETIF_ASSERT(adapter, link, adapter->netdev, adapter);
			phy = &adapter->phy;

			memset(&ksettings, 0, sizeof(ksettings));
			phy_ethtool_get_link_ksettings(netdev, &ksettings);

			local_advertisement = phy_read(phydev, MII_ADVERTISE);
			if (local_advertisement < 0) {
				NETIF_ERROR(adapter, link, adapter->netdev,
					    "reading local_advertisement failed");
				goto done;
			}

			remote_advertisement = phy_read(phydev, MII_LPA);
			if (remote_advertisement < 0) {
				NETIF_ERROR(adapter, link, adapter->netdev,
					    "reading remote_advertisement failed");
				goto done;
			}

			NETIF_INFO(adapter, link, adapter->netdev,
				   "link UP: speed: %u duplex: %d anadv: 0x%04x anlpa: 0x%04x",
				   ksettings.base.speed, ksettings.base.duplex,
				   local_advertisement, remote_advertisement);

			lan743x_phy_update_flowcontrol(adapter,
						       ksettings.base.duplex,
						       local_advertisement,
						       remote_advertisement);
		} else if (phydev->state == PHY_NOLINK) {
			NETIF_INFO(adapter, link, adapter->netdev,
				   "link DOWN");
		}
	} else {
		NETIF_ERROR(adapter, link, adapter->netdev, "phydev == NULL");
	}
done:
	return;
}

static int lan743x_phy_open(struct lan743x_adapter *adapter)
{
	struct lan743x_phy *phy = &adapter->phy;
	int ret;
	u32 mii_adv;
	struct phy_device *phydev;
	struct net_device *netdev;
	struct lan743x_mac *mac = &adapter->mac;
	int phy_id1 = 0;
	int phy_id2 = 0;

	netdev = adapter->netdev;

	NETIF_ASSERT(adapter, ifup, adapter->netdev, mac->mdiobus);

	phydev = phy_find_first(mac->mdiobus);
	if (!phydev) {
		NETIF_ERROR(adapter, ifup, adapter->netdev, "no PHY found");
		ret = -EIO;
		goto clean_up;
	}

	phydev->irq = PHY_POLL;

	NETIF_INFO(adapter, ifup, adapter->netdev,
		   "phy irq assigned to %d", phydev->irq);
	ret = phy_connect_direct(netdev, phydev,
				 lan743x_phy_link_status_change,
				 PHY_INTERFACE_MODE_GMII);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "can't attach PHY to %s", mac->mdiobus->id);
		ret = -EIO;
		goto clean_up;
	}
	phy->flags |= PHY_FLAG_ATTACHED;

	if (!(phydev->drv)) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "Missing PHY Driver");
		ret = -EIO;
		goto clean_up;
	}

	phy_id1 = phy_read(phydev, MII_PHYSID1);
	phy_id2 = phy_read(phydev, MII_PHYSID2);
	NETIF_INFO(adapter, ifup, adapter->netdev,
		   "PHY_ID1 = 0x%04x", phy_id1);
	NETIF_INFO(adapter, ifup, adapter->netdev,
		   "PHY_ID2 = 0x%04x", phy_id2);

	/* MAC doesn't support 1000T Half */
	phydev->supported &= ~SUPPORTED_1000baseT_Half;

	/* support both flow controls */
	phy->fc_request_control = (FLOW_CTRL_RX | FLOW_CTRL_TX);
	phydev->advertising &= ~(ADVERTISED_Pause | ADVERTISED_Asym_Pause);
	mii_adv = (u32)mii_advertise_flowctrl(phy->fc_request_control);
	phydev->advertising |= mii_adv_to_ethtool_adv_t(mii_adv);

	phy->fc_autoneg = phydev->autoneg;

	/* PHY interrupt enabled here */
	phy_start(phydev);

	phy_start_aneg(phydev);

	phy->flags |= PHY_FLAG_OPENED;
	ret = 0;

clean_up:
	if (ret)
		lan743x_phy_close(adapter);
	return ret;
}

static void lan743x_phy_close(struct lan743x_adapter *adapter)
{
	struct lan743x_phy *phy = &adapter->phy;
	struct net_device *netdev = adapter->netdev;

	if (phy->flags & PHY_FLAG_OPENED) {
		netif_carrier_off(netdev);

		phy_stop(netdev->phydev);
		phy->flags &= ~PHY_FLAG_OPENED;
	}
	if (phy->flags & PHY_FLAG_ATTACHED) {
		phy_disconnect(netdev->phydev);
		netdev->phydev = NULL;
		phy->flags &= ~PHY_FLAG_ATTACHED;
	}
}

/* RFE */

static void lan743x_rfe_update_mac_address(struct lan743x_adapter *adapter)
{
	u8 mac_addr[ETH_ALEN];
	u32 mac_addr_hi = 0;
	u32 mac_addr_lo = 0;

	/* Add mac address to perfect Filter */
	lan743x_mac_get_address(adapter, mac_addr);
	mac_addr_lo = ((((u32)(mac_addr[0])) << 0) |
		      (((u32)(mac_addr[1])) << 8) |
		      (((u32)(mac_addr[2])) << 16) |
		      (((u32)(mac_addr[3])) << 24));
	mac_addr_hi = ((((u32)(mac_addr[4])) << 0) |
		      (((u32)(mac_addr[5])) << 8));
	lan743x_csr_write(adapter, RFE_ADDR_FILT_LO(0), mac_addr_lo);
	lan743x_csr_write(adapter, RFE_ADDR_FILT_HI(0),
			  mac_addr_hi | RFE_ADDR_FILT_HI_VALID_);
}

static int lan743x_rfe_init(struct lan743x_adapter *adapter)
{
	struct lan743x_rfe *rfe = &adapter->rfe;

	NETIF_ASSERT(adapter, probe, adapter->netdev, rfe);
	memset(rfe, 0, sizeof(*rfe));

	/* Add mac address to perfect Filter */
	lan743x_rfe_update_mac_address(adapter);

	return 0;
}

static void lan743x_rfe_cleanup(struct lan743x_adapter *adapter)
{
	/* This empty function is kept as a place holder */
}

static int lan743x_rfe_open(struct lan743x_adapter *adapter)
{
	/* This empty function is kept as a place holder */
	return 0;
}

static void lan743x_rfe_close(struct lan743x_adapter *adapter)
{
	/* This empty function is kept as a place holder */
}

/* returns hash bit number for given MAC address */
static inline u32 lan743x_rfe_get_hash_bit(char addr[ETH_ALEN])
{
	return (ether_crc(ETH_ALEN, addr) >> 23) & 0x1ff;
}

static void lan743x_rfe_set_multicast(struct lan743x_adapter *adapter)
{
	u32 rfctl;
	u32 data;
	struct net_device *netdev = adapter->netdev;
	u32 hash_table[DP_SEL_VHF_HASH_LEN];

	rfctl = lan743x_csr_read(adapter, RFE_CTL);

	rfctl &= ~(RFE_CTL_AU_ | RFE_CTL_AM_ |
		 RFE_CTL_DA_PERFECT_ | RFE_CTL_MCAST_HASH_);

	rfctl |= RFE_CTL_AB_;

	if (netdev->flags & IFF_PROMISC) {
		rfctl |= RFE_CTL_AM_ | RFE_CTL_AU_;
	} else {
		if (netdev->flags & IFF_ALLMULTI)
			rfctl |= RFE_CTL_AM_;
	}

	memset(hash_table, 0, DP_SEL_VHF_HASH_LEN * sizeof(u32));

	if (netdev_mc_count(netdev)) {
		struct netdev_hw_addr *ha;
		int i;

		rfctl |= RFE_CTL_DA_PERFECT_;

		i = 1;
		netdev_for_each_mc_addr(ha, netdev) {
			/* set first 32 into Perfect Filter */
			if (i < 33) {
				lan743x_csr_write(adapter,
						  RFE_ADDR_FILT_HI(i), 0);
				data = ha->addr[3];
				data = ha->addr[2] | (data << 8);
				data = ha->addr[1] | (data << 8);
				data = ha->addr[0] | (data << 8);
				lan743x_csr_write(adapter,
						  RFE_ADDR_FILT_LO(i), data);
				data = ha->addr[5];
				data = ha->addr[4] | (data << 8);
				data |= RFE_ADDR_FILT_HI_VALID_;
				lan743x_csr_write(adapter,
						  RFE_ADDR_FILT_HI(i), data);
			} else {
				u32 bitnum = lan743x_rfe_get_hash_bit(ha->addr);

				hash_table[bitnum / 32] |= (1 << (bitnum % 32));
				rfctl |= RFE_CTL_MCAST_HASH_;
			}
			i++;
		}
	}

	if (lan743x_dp_write_hash_filter(adapter, hash_table))
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "write to hash table failed");

	lan743x_csr_write(adapter, RFE_CTL, rfctl);
}

/* FCT */

static void lan743x_fct_isr(void *context)
{
	struct lan743x_adapter *adapter = (struct lan743x_adapter *)context;
	u32 fct_int_sts = 0;
	u32 fct_int_en = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, adapter);

	lan743x_csr_write(adapter, INT_EN_CLR, INT_BIT_FCT_);

	fct_int_sts = lan743x_csr_read(adapter, FCT_INT_STS);
	fct_int_en = lan743x_csr_read(adapter, FCT_INT_EN_SET);

	fct_int_sts = fct_int_sts & fct_int_en;
	if (fct_int_sts & FCT_INT_MASK_ERRORS_) {
		if (fct_int_sts & FCT_INT_BIT_TXE_)
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Transmitter Error");
		if (fct_int_sts & FCT_INT_BIT_TDFO_)
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Tx Data FIFO Overrun");
		if (fct_int_sts & FCT_INT_BIT_TDFU_)
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "TX Data FIFO Underrun");
	}
	if (fct_int_sts & (~FCT_INT_MASK_ERRORS_))
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "unhandled interrupt, fct_int_sts = 0x%08X",
			    fct_int_sts);

	/* clear fct int status bits */
	lan743x_csr_write(adapter, FCT_INT_STS, fct_int_sts);

	/* enable isr */
	lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_FCT_);
}

static int lan743x_fct_init(struct lan743x_adapter *adapter)
{
	/* this empty function is kept as a place holder */
	return 0;
}

static void lan743x_fct_cleanup(struct lan743x_adapter *adapter)
{
	/* this empty function is kept as a place holder */
}

static int lan743x_fct_open(struct lan743x_adapter *adapter)
{
	lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_FCT_);
	lan743x_csr_write(adapter, FCT_INT_EN_SET, FCT_INT_MASK_ERRORS_);
	return 0;
}

static void lan743x_fct_close(struct lan743x_adapter *adapter)
{
	lan743x_csr_write(adapter, FCT_INT_EN_CLR, FCT_INT_MASK_ERRORS_);
	lan743x_csr_write(adapter, INT_EN_CLR, INT_BIT_FCT_);
}

static int lan743x_fct_rx_reset(struct lan743x_adapter *adapter, int rx_channel)
{
	int ret = -EIO;
	int timeout = 100;
	u32 data = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (rx_channel >= 0) &&
		     (rx_channel < LAN743X_NUMBER_OF_RX_CHANNELS));

	data = lan743x_csr_read(adapter, FCT_RX_CTL);
	if (data & FCT_RX_CTL_EN_(rx_channel)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Attempting to reset fifo while enabled, rx_channel = %d",
			    rx_channel);
		ret = -EIO;
		goto done;
	}

	lan743x_csr_write(adapter, FCT_RX_CTL, FCT_RX_CTL_RESET_(rx_channel));
	while (timeout &&
	       ((data = lan743x_csr_read(adapter, FCT_RX_CTL)) &
	       FCT_RX_CTL_RESET_(rx_channel))) {
		usleep_range(1000, 20000);
		timeout--;
	}
	if (data & FCT_RX_CTL_RESET_(rx_channel)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Timed out waiting for rx fifo to reset, rx_channel = %d",
			    rx_channel);
		ret = -EIO;
		goto done;
	}

	lan743x_csr_write(adapter, FCT_FLOW(rx_channel),
			  FCT_FLOW_CTL_REQ_EN_ |
			  FCT_FLOW_CTL_ON_THRESHOLD_SET_(0x2A) |
			  FCT_FLOW_CTL_OFF_THRESHOLD_SET_(0xA));

	ret = 0;
done:
	return ret;
}

static int lan743x_fct_rx_enable(struct lan743x_adapter *adapter,
				 int rx_channel)
{
	u32 data = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (rx_channel >= 0) &&
		     (rx_channel < LAN743X_NUMBER_OF_RX_CHANNELS));

	data = lan743x_csr_read(adapter, FCT_RX_CTL);
	if (data & FCT_RX_CTL_EN_(rx_channel)) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Attempting to enable an already enabled channel, rx_channel = %d",
			      rx_channel);
	} else {
		lan743x_csr_write(adapter, FCT_RX_CTL,
				  FCT_RX_CTL_EN_(rx_channel));
	}

	return 0;
}

static int lan743x_fct_rx_disable(struct lan743x_adapter *adapter,
				  int rx_channel)
{
	int ret = -EIO;
	int timeout = 100;
	u32 data = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     (rx_channel >= 0) &&
		     (rx_channel < LAN743X_NUMBER_OF_RX_CHANNELS));

	data = lan743x_csr_read(adapter, FCT_RX_CTL);
	if (!(data & FCT_RX_CTL_EN_(rx_channel))) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Attempting to disable an already disabled channel, rx_channel = %d",
			      rx_channel);
		ret = 0;
		goto done;
	} else {
		lan743x_csr_write(adapter, FCT_RX_CTL,
				  FCT_RX_CTL_DIS_(rx_channel));
	}

	while (timeout &&
	       ((data = lan743x_csr_read(adapter, FCT_RX_CTL)) &
	       FCT_RX_CTL_EN_(rx_channel))) {
		usleep_range(1000, 20000);
		timeout--;
	}
	if (data & FCT_RX_CTL_EN_(rx_channel)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Timed out waiting for rx fifo to disable, rx_channel = %d",
			    rx_channel);
		ret = -EIO;
		goto done;
	}
	ret = 0;
done:
	return ret;
}

static int lan743x_fct_tx_reset(struct lan743x_adapter *adapter, int tx_channel)
{
	int ret = -EIO;
	int timeout = 100;
	u32 data = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (tx_channel >= 0) &&
		     (tx_channel < LAN743X_NUMBER_OF_TX_CHANNELS));

	data = lan743x_csr_read(adapter, FCT_TX_CTL);
	if (data & FCT_TX_CTL_EN_(tx_channel)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Attempting to reset fifo while enabled, tx_channel = %d",
			    tx_channel);
		ret = -EIO;
		goto done;
	}

	lan743x_csr_write(adapter, FCT_TX_CTL, FCT_TX_CTL_RESET_(tx_channel));
	while (timeout &&
	       ((data = lan743x_csr_read(adapter, FCT_TX_CTL)) &
	       FCT_TX_CTL_RESET_(tx_channel)))	{
		usleep_range(1000, 20000);
		timeout--;
	}
	if (data & FCT_TX_CTL_RESET_(tx_channel)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Timed out waiting for tx fifo to reset, tx_channel = %d",
			    tx_channel);
		ret = -EIO;
		goto done;
	}
	ret = 0;
done:
	return ret;
}

static int lan743x_fct_tx_enable(struct lan743x_adapter *adapter,
				 int tx_channel)
{
	u32 data = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (tx_channel >= 0) &&
		     (tx_channel < LAN743X_NUMBER_OF_TX_CHANNELS));

	data = lan743x_csr_read(adapter, FCT_TX_CTL);
	if (data & FCT_TX_CTL_EN_(tx_channel)) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Attempting to enable an already enabled channel, tx_channel = %d",
			      tx_channel);
	} else {
		lan743x_csr_write(adapter, FCT_TX_CTL,
				  FCT_TX_CTL_EN_(tx_channel));
	}

	return 0;
}

static int lan743x_fct_tx_disable(struct lan743x_adapter *adapter,
				  int tx_channel)
{
	int ret = -EIO;
	int timeout = 100;
	u32 data = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (tx_channel >= 0) &&
		     (tx_channel < LAN743X_NUMBER_OF_TX_CHANNELS));

	data = lan743x_csr_read(adapter, FCT_TX_CTL);
	if (!(data & FCT_TX_CTL_EN_(tx_channel))) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Attempting to disable an already disabled channel, tx_channel = %d",
			      tx_channel);
		ret = 0;
		goto done;
	} else {
		lan743x_csr_write(adapter, FCT_TX_CTL,
				  FCT_TX_CTL_DIS_(tx_channel));
	}

	while (timeout &&
	       ((data = lan743x_csr_read(adapter, FCT_TX_CTL)) &
	       FCT_TX_CTL_EN_(tx_channel))) {
		usleep_range(1000, 20000);
		timeout--;
	}
	if (data & FCT_TX_CTL_EN_(tx_channel)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Timed out waiting for tx fifo to disable, tx_channel = %d",
			    tx_channel);
		ret = -EIO;
		goto done;
	}
	ret = 0;
done:
	return ret;
}

/* DMAC */

#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
#define DMA_ADDR_HIGH32(dma_addr)   ((u32)(((dma_addr) >> 32) & 0xFFFFFFFF))
#else
#define DMA_ADDR_HIGH32(dma_addr)   ((u32)(0))
#endif
#define DMA_ADDR_LOW32(dma_addr) ((u32)((dma_addr) & 0xFFFFFFFF))

#define DMAC_FLAG_TX_USED(channel)  BIT(0 + (channel))
#define DMAC_FLAG_TX0_USED      BIT(0)
#define DMAC_FLAG_TX1_USED      BIT(1)
#define DMAC_FLAG_TX2_USED      BIT(2)
#define DMAC_FLAG_TX3_USED      BIT(3)
#define DMAC_FLAG_RX_USED(channel)  BIT(4 + (channel))
#define DMAC_FLAG_RX0_USED      BIT(4)
#define DMAC_FLAG_RX1_USED      BIT(5)
#define DMAC_FLAG_RX2_USED      BIT(6)
#define DMAC_FLAG_RX3_USED      BIT(7)

#define DMA_DESCRIPTOR_SPACING_16       (16)
#define DMA_DESCRIPTOR_SPACING_32       (32)
#define DMA_DESCRIPTOR_SPACING_64       (64)
#define DMA_DESCRIPTOR_SPACING_128      (128)

#define DEFAULT_DMA_DESCRIPTOR_SPACING  (L1_CACHE_BYTES)

static void lan743x_dmac_isr(void *context)
{
	struct lan743x_adapter *adapter = (struct lan743x_adapter *)context;
	struct lan743x_dmac *dmac = NULL;
	u32 dmac_int_sts = 0;
	int channel = 0;
	int found_set_bit = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, adapter);
	dmac = &adapter->dmac;

	lan743x_csr_write(adapter, INT_EN_CLR, INT_BIT_DMA_GEN_);

	dmac_int_sts = lan743x_csr_read(adapter, DMAC_INT_STS);

	if (!(dmac_int_sts & DMAC_INT_BIT_ERR_)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "unexpected dmac_isr call");
		goto done;
	}

	for (channel = 0; channel < LAN743X_NUMBER_OF_RX_CHANNELS;
	     channel++) {
		u32 rx_err_sts = lan743x_csr_read(adapter,
						  DMAC_RX_ERR_STS(channel));

		if (rx_err_sts &
		    (DMAC_RX_ERR_STS_RESERVED_ |
		    DMAC_RX_ERR_STS_RX_DESC_READ_ERR_ |
		    DMAC_RX_ERR_STS_RX_DESC_TAIL_ERR_)) {
			found_set_bit = 1;
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "RX_ERR_STS(%d) = 0x%08X",
				    channel, rx_err_sts);
			if (rx_err_sts & DMAC_RX_ERR_STS_RESERVED_)
				NETIF_ERROR(adapter, drv, adapter->netdev,
					    "  reserved bits set");
			if (rx_err_sts &
			    DMAC_RX_ERR_STS_RX_DESC_READ_ERR_)
				NETIF_ERROR(adapter, drv, adapter->netdev,
					    "  RX Descriptor Read Retry Error");
			if (rx_err_sts &
			    DMAC_RX_ERR_STS_RX_DESC_TAIL_ERR_)
				NETIF_ERROR(adapter, drv, adapter->netdev,
					    "  RX Descriptor Tail Error");

			/* clear errors */
			lan743x_csr_write(adapter, DMAC_RX_ERR_STS(channel),
					  rx_err_sts);
		}
	}
	for (channel = 0;
	     channel < LAN743X_NUMBER_OF_TX_CHANNELS; channel++) {
		u32 tx_err_sts = lan743x_csr_read(adapter,
						  DMAC_TX_ERR_STS(channel));

		if (tx_err_sts &
		    (DMAC_TX_ERR_STS_RESERVED_ |
		    DMAC_TX_ERR_STS_TX_DATA_READ_ERR_ |
		    DMAC_TX_ERR_STS_TX_DESC_READ_ERR_ |
		    DMAC_TX_ERR_STS_TX_DESC_TAIL_ERR_ |
		    DMAC_TX_ERR_STS_TX_FCT_TXE_ |
		    DMAC_TX_ERR_STS_TX_DESC_DATATYPE_ERR_ |
		    DMAC_TX_ERR_STS_TX_DESC_EXTNTYPE_ERR_ |
		    DMAC_TX_ERR_STS_TX_DESC_EXTRAFS_ERR_ |
		    DMAC_TX_ERR_STS_TX_DESC_NOFS_ERR_)) {
			found_set_bit = 1;
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "TX_ERR_STS(%d) = 0x%08X",
				    channel, tx_err_sts);
			if (tx_err_sts & DMAC_TX_ERR_STS_RESERVED_)
				NETIF_ERROR(adapter, drv, adapter->netdev,
					    "  reserved bits set");
			if (tx_err_sts &
			    DMAC_TX_ERR_STS_TX_DATA_READ_ERR_)
				NETIF_ERROR(adapter, drv, adapter->netdev,
					    "  TX Data Buffer Read Retry Error");
			if (tx_err_sts &
			    DMAC_TX_ERR_STS_TX_DESC_READ_ERR_)
				NETIF_ERROR(adapter, drv, adapter->netdev,
					    "  TX Descriptor Read Retry Error");
			if (tx_err_sts &
			    DMAC_TX_ERR_STS_TX_DESC_TAIL_ERR_)
				NETIF_ERROR(adapter, drv, adapter->netdev,
					    "  TX Descriptor Tail Error");
			if (tx_err_sts & DMAC_TX_ERR_STS_TX_FCT_TXE_)
				NETIF_ERROR(adapter, drv, adapter->netdev,
					    "  TX FCT TX Error");
			if (tx_err_sts &
			    DMAC_TX_ERR_STS_TX_DESC_DATATYPE_ERR_)
				NETIF_ERROR(adapter, drv, adapter->netdev,
					    "  TX Data Descriptor Missing Error");
			if (tx_err_sts &
			    DMAC_TX_ERR_STS_TX_DESC_EXTNTYPE_ERR_)
				NETIF_ERROR(adapter, drv, adapter->netdev,
					    "  TX Extension Descriptor Missing Error");
			if (tx_err_sts &
			    DMAC_TX_ERR_STS_TX_DESC_EXTRAFS_ERR_)
				NETIF_ERROR(adapter, drv, adapter->netdev,
					    "  TX Descriptor Extraneous FS Error");
			if (tx_err_sts &
			    DMAC_TX_ERR_STS_TX_DESC_NOFS_ERR_)
				NETIF_ERROR(adapter, drv, adapter->netdev,
					    "  TX Descriptor Missing FS Error");

			/* clear errors */
			lan743x_csr_write(adapter, DMAC_TX_ERR_STS(channel),
					  tx_err_sts);
		}
	}
	if (!found_set_bit)
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "DMAC_INT_BIT_ERR_ set with out cause, DMAC_INT_STS = 0x%08X",
			    dmac_int_sts);
done:
	/* clear dma int status */
	lan743x_csr_write(adapter, DMAC_INT_STS, dmac_int_sts);

	/* enable isr */
	lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_DMA_GEN_);
}

static int lan743x_dmac_reset(struct lan743x_adapter *adapter)
{
	int timeout = 100;
	u32 data = 0;
	int ret = 0;

	lan743x_csr_write(adapter, DMAC_CMD, DMAC_CMD_SWR_);
	while (timeout &&
	       ((data = lan743x_csr_read(adapter, DMAC_CMD)) &
	       DMAC_CMD_SWR_)) {
		usleep_range(1000, 20000);
		timeout--;
	}
	if (data & DMAC_CMD_SWR_) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Timed out waiting for DMAC reset to complete");
		ret = -ENODEV;
	}
	return ret;
}

static int lan743x_dmac_init(struct lan743x_adapter *adapter)
{
	struct lan743x_dmac *dmac = &adapter->dmac;
	int ret = -ENODEV;
	u32 dma_cfg = 0;

	NETIF_ASSERT(adapter, probe, adapter->netdev, dmac);

	memset(dmac, 0, sizeof(*dmac));

	dmac->flags = 0;

	dmac->descriptor_spacing = DEFAULT_DMA_DESCRIPTOR_SPACING;

	ret = lan743x_dmac_reset(adapter);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "DMAC reset failed");
		goto cleanup;
	}

	switch (dmac->descriptor_spacing) {
	case DMA_DESCRIPTOR_SPACING_16:
		dma_cfg = DMAC_CFG_MAX_DSPACE_16_;
		break;
	case DMA_DESCRIPTOR_SPACING_32:
		dma_cfg = DMAC_CFG_MAX_DSPACE_32_;
		break;
	case DMA_DESCRIPTOR_SPACING_64:
		dma_cfg = DMAC_CFG_MAX_DSPACE_64_;
		break;
	case DMA_DESCRIPTOR_SPACING_128:
		dma_cfg = DMAC_CFG_MAX_DSPACE_128_;
		break;
	default:
		ret = -EPERM;
		goto cleanup;
	}
	dma_cfg |= DMAC_CFG_CH_ARB_SEL_RX_HIGH_;
	dma_cfg |= DMAC_CFG_MAX_READ_REQ_SET_(6);
	lan743x_csr_write(adapter, DMAC_CFG, dma_cfg);

	ret = 0;
cleanup:
	if (ret)
		lan743x_dmac_cleanup(adapter);
	return ret;
}

static void lan743x_dmac_cleanup(struct lan743x_adapter *adapter)
{
	struct lan743x_dmac *dmac = &adapter->dmac;
	int channel;

	/* error checking */
	for (channel = 0; channel < LAN743X_NUMBER_OF_TX_CHANNELS;
	     channel++) {
		if (dmac->flags & (DMAC_FLAG_TX_USED(channel))) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "TX Channel %d, is still in use",
				    channel);
		}
	}
	for (channel = 0; channel < LAN743X_NUMBER_OF_RX_CHANNELS;
	     channel++) {
		if (dmac->flags & (DMAC_FLAG_RX_USED(channel))) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "RX Channel %d, is still in use",
				    channel);
		}
	}

	memset(dmac, 0, sizeof(*dmac));
}

static int lan743x_dmac_open(struct lan743x_adapter *adapter)
{
	lan743x_csr_write(adapter, INT_EN_SET, INT_BIT_DMA_GEN_);
	lan743x_csr_write(adapter, DMAC_INT_EN_SET, DMAC_INT_BIT_ERR_);

	return 0;
}

static void lan743x_dmac_close(struct lan743x_adapter *adapter)
{
	lan743x_csr_write(adapter, DMAC_INT_EN_CLR, DMAC_INT_BIT_ERR_);
	lan743x_csr_write(adapter, INT_EN_CLR, INT_BIT_DMA_GEN_);
}

static int lan743x_dmac_get_descriptor_spacing(struct lan743x_adapter *adapter)
{
	struct lan743x_dmac *dmac = &adapter->dmac;

	return dmac->descriptor_spacing;
}

static int lan743x_dmac_reserve_tx_channel(struct lan743x_adapter *adapter,
					   int tx_channel)
{
	struct lan743x_dmac *dmac = &adapter->dmac;
	int ret = -EBUSY;

	if ((tx_channel >= 0) && (tx_channel < LAN743X_NUMBER_OF_TX_CHANNELS)) {
		if (!(dmac->flags & DMAC_FLAG_TX_USED(tx_channel))) {
			/* tx channel not yet used, go ahead and reserve it */
			dmac->flags |= DMAC_FLAG_TX_USED(tx_channel);
			ret = 0;
		} else {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Attempted to reserve a channel that was already reserved, tx_channel = %d",
				    tx_channel);
		}
	} else {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "out of range, channel_number = %d",
			    tx_channel);
	}
	return ret;
}

static void lan743x_dmac_release_tx_channel(struct lan743x_adapter *adapter,
					    int tx_channel)
{
	struct lan743x_dmac *dmac = &adapter->dmac;

	if ((tx_channel >= 0) && (tx_channel < LAN743X_NUMBER_OF_TX_CHANNELS)) {
		if (dmac->flags & DMAC_FLAG_TX_USED(tx_channel)) {
			/* tx channel is in use, go ahead and release it */
			dmac->flags &= ~DMAC_FLAG_TX_USED(tx_channel);
		} else {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Attempted to release a tx channel that was not in use, tx_channel = %d",
				    tx_channel);
		}
	} else {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "out of range, tx_channel = %d",
			    tx_channel);
	}
}

static int lan743x_dmac_reserve_rx_channel(struct lan743x_adapter *adapter,
					   int rx_channel)
{
	struct lan743x_dmac *dmac = &adapter->dmac;
	int ret = -EBUSY;

	if ((rx_channel >= 0) && (rx_channel < LAN743X_NUMBER_OF_RX_CHANNELS)) {
		if (!(dmac->flags & DMAC_FLAG_RX_USED(rx_channel))) {
			/* rx channel not yet used, go ahead and reserve it */
			dmac->flags |= DMAC_FLAG_RX_USED(rx_channel);
			ret = 0;
		} else {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Attempted to reserve an rx channel that was already reserved, rx_channel = %d",
				    rx_channel);
		}
	} else {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "out of range, rx_channel = %d", rx_channel);
	}
	return ret;
}

static void lan743x_dmac_release_rx_channel(struct lan743x_adapter *adapter,
					    int rx_channel)
{
	struct lan743x_dmac *dmac = &adapter->dmac;

	if ((rx_channel >= 0) && (rx_channel < LAN743X_NUMBER_OF_RX_CHANNELS)) {
		if (dmac->flags & DMAC_FLAG_RX_USED(rx_channel)) {
			/* rx channel is in use, go ahead and release it */
			dmac->flags &= ~DMAC_FLAG_RX_USED(rx_channel);
		} else {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Attempted to release an rx channel that was not in use, rx_channel = %d",
				    rx_channel);
		}
	} else {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "out of range, rx_channel = %d", rx_channel);
	}
}

#define DMAC_CHANNEL_STATE_SET(start_bit, stop_bit) \
	(((start_bit) ? 2 : 0) | ((stop_bit) ? 1 : 0))
#define DMAC_CHANNEL_STATE_INITIAL      DMAC_CHANNEL_STATE_SET(0, 0)
#define DMAC_CHANNEL_STATE_STARTED      DMAC_CHANNEL_STATE_SET(1, 0)
#define DMAC_CHANNEL_STATE_STOP_PENDING DMAC_CHANNEL_STATE_SET(1, 1)
#define DMAC_CHANNEL_STATE_STOPPED      DMAC_CHANNEL_STATE_SET(0, 1)

static int lan743x_dmac_tx_reset(struct lan743x_adapter *adapter,
				 int tx_channel)
{
	struct lan743x_dmac *dmac = &adapter->dmac;
	int ret = 0;
	int timeout = 100;
	u32 data = 0;
	u32 reset_bit = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (tx_channel >= 0) &&
		     (tx_channel < LAN743X_NUMBER_OF_TX_CHANNELS));
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     dmac->flags & DMAC_FLAG_TX_USED(tx_channel));

	reset_bit = DMAC_CMD_TX_SWR_(tx_channel);

	lan743x_csr_write(adapter, DMAC_CMD, reset_bit);
	while (timeout &&
	       ((data = lan743x_csr_read(adapter, DMAC_CMD)) & reset_bit)) {
		usleep_range(1000, 20000);
		timeout--;
	}
	if (data & reset_bit) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Timed out waiting for TX channel %d reset to complete",
			    tx_channel);
		ret = -ENODEV;
	}
	return ret;
}

static int lan743x_dmac_tx_get_state(struct lan743x_adapter *adapter,
				     int tx_channel)
{
	struct lan743x_dmac *dmac = &adapter->dmac;
	u32 dmac_cmd = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (tx_channel >= 0) &&
		     (tx_channel < LAN743X_NUMBER_OF_TX_CHANNELS));
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     dmac->flags & DMAC_FLAG_TX_USED(tx_channel));

	dmac_cmd = lan743x_csr_read(adapter, DMAC_CMD);
	return DMAC_CHANNEL_STATE_SET((dmac_cmd &
				      DMAC_CMD_START_T_(tx_channel)),
				      (dmac_cmd &
				      DMAC_CMD_STOP_T_(tx_channel)));
}

static int lan743x_dmac_tx_wait_till_stopped(struct lan743x_adapter *adapter,
					     int tx_channel)
{
	int result = 0;
	int timeout = 100;

	while (timeout &&
	       ((result = lan743x_dmac_tx_get_state(adapter, tx_channel)) ==
	       DMAC_CHANNEL_STATE_STOP_PENDING)) {
		usleep_range(1000, 20000);
		timeout--;
	}
	if (result == DMAC_CHANNEL_STATE_STOP_PENDING) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Timed out waiting for tx channel %d to stop",
			    tx_channel);
		result = -ENODEV;
	}
	return result;
}

static int lan743x_dmac_tx_start(struct lan743x_adapter *adapter,
				 int tx_channel)
{
	struct lan743x_dmac *dmac = &adapter->dmac;
	int ret = 0;
	int state = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (tx_channel >= 0) &&
		     (tx_channel < LAN743X_NUMBER_OF_TX_CHANNELS));
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     dmac->flags & DMAC_FLAG_TX_USED(tx_channel));

	state = lan743x_dmac_tx_wait_till_stopped(adapter, tx_channel);
	if (state < 0) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "failed wait till not stop pending, tx_channel = %d",
			    tx_channel);
		ret = -ENODEV;
		goto done;
	}
	if (state != DMAC_CHANNEL_STATE_STARTED) {
		lan743x_csr_write(adapter, DMAC_CMD,
				  DMAC_CMD_START_T_(tx_channel));
		state = lan743x_dmac_tx_get_state(adapter, tx_channel);
		if (state != DMAC_CHANNEL_STATE_STARTED) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Failed to start tx channel %d",
				    tx_channel);
			ret = -ENODEV;
			goto done;
		}
	} else {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Attempting to start an already started tx_channel = %d",
			      tx_channel);
	}
done:
	return ret;
}

static int lan743x_dmac_tx_stop(struct lan743x_adapter *adapter,
				int tx_channel)
{
	struct lan743x_dmac *dmac = &adapter->dmac;
	int ret = 0;
	int state = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (tx_channel >= 0) &&
		     (tx_channel < LAN743X_NUMBER_OF_TX_CHANNELS));
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     dmac->flags & DMAC_FLAG_TX_USED(tx_channel));

	state = lan743x_dmac_tx_get_state(adapter, tx_channel);
	if (state == DMAC_CHANNEL_STATE_STARTED) {
		lan743x_csr_write(adapter,
				  DMAC_CMD, DMAC_CMD_STOP_T_(tx_channel));
		state = lan743x_dmac_tx_wait_till_stopped(
			adapter, tx_channel);
		if (state < 0) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "failed wait till not stop pending, tx_channel = %d",
				    tx_channel);
			ret = -ENODEV;
		}
	} else if (state == DMAC_CHANNEL_STATE_STOP_PENDING) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "A stop is already pending for tx_channel = %d",
			    tx_channel);
		state = lan743x_dmac_tx_wait_till_stopped(
			adapter, tx_channel);
		if (state < 0) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "failed wait till not stop pending, tx_channel = %d",
				    tx_channel);
			ret = -ENODEV;
		}
	} else {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Attempting to stop a not started tx channel = %d",
			      tx_channel);
	}
	return ret;
}

static int lan743x_dmac_rx_get_state(struct lan743x_adapter *adapter,
				     int rx_channel)
{
	struct lan743x_dmac *dmac = &adapter->dmac;
	u32 dmac_cmd = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (rx_channel >= 0) &&
		     (rx_channel < LAN743X_NUMBER_OF_RX_CHANNELS));
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     dmac->flags & DMAC_FLAG_RX_USED(rx_channel));

	dmac_cmd = lan743x_csr_read(adapter, DMAC_CMD);
	return DMAC_CHANNEL_STATE_SET((dmac_cmd &
				      DMAC_CMD_START_R_(rx_channel)),
				      (dmac_cmd &
				      DMAC_CMD_STOP_R_(rx_channel)));
}

static int lan743x_dmac_rx_wait_till_stopped(struct lan743x_adapter *adapter,
					     int rx_channel)
{
	int result = 0;
	int timeout = 100;

	while (timeout &&
	       ((result = lan743x_dmac_rx_get_state(adapter, rx_channel)) ==
	       DMAC_CHANNEL_STATE_STOP_PENDING)) {
		usleep_range(1000, 20000);
		timeout--;
	}
	if (result == DMAC_CHANNEL_STATE_STOP_PENDING) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Timed out waiting for rx channel %d to stop",
			    rx_channel);
		result = -ENODEV;
	}
	return result;
}

static int lan743x_dmac_rx_reset(struct lan743x_adapter *adapter,
				 int rx_channel)
{
	struct lan743x_dmac *dmac = &adapter->dmac;
	int ret = 0;
	int timeout = 100;
	u32 data = 0;
	u32 reset_bit = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (rx_channel >= 0) &&
		     (rx_channel < LAN743X_NUMBER_OF_RX_CHANNELS));
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     dmac->flags & DMAC_FLAG_RX_USED(rx_channel));

	reset_bit = DMAC_CMD_RX_SWR_(rx_channel);

	lan743x_csr_write(adapter, DMAC_CMD, reset_bit);
	while (timeout &&
	       ((data = lan743x_csr_read(adapter, DMAC_CMD)) & reset_bit)) {
		usleep_range(1000, 20000);
		timeout--;
	}
	if (data & reset_bit) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Timed out waiting for RX channel %d reset to complete",
			    rx_channel);
		ret = -ENODEV;
	}
	return ret;
}

static int lan743x_dmac_rx_start(struct lan743x_adapter *adapter,
				 int rx_channel)
{
	struct lan743x_dmac *dmac = &adapter->dmac;
	int ret = 0;
	int state = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (rx_channel >= 0) &&
		     (rx_channel < LAN743X_NUMBER_OF_RX_CHANNELS));
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     dmac->flags & DMAC_FLAG_RX_USED(rx_channel));

	state = lan743x_dmac_rx_wait_till_stopped(adapter, rx_channel);
	if (state < 0) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "failed wait till not stop pending, rx_channel = %d",
			    rx_channel);
		ret = -ENODEV;
		goto done;
	}
	if (state != DMAC_CHANNEL_STATE_STARTED) {
		lan743x_csr_write(adapter, DMAC_CMD,
				  DMAC_CMD_START_R_(rx_channel));
		state = lan743x_dmac_rx_get_state(adapter, rx_channel);
		if (state != DMAC_CHANNEL_STATE_STARTED) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Failed to start rx channel %d",
				    rx_channel);
			ret = -ENODEV;
			goto done;
		}
	} else {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Attempting to start an already started rx_channel = %d",
			      rx_channel);
	}
done:
	return ret;
}

static int lan743x_dmac_rx_stop(struct lan743x_adapter *adapter,
				int rx_channel)
{
	struct lan743x_dmac *dmac = &adapter->dmac;
	int ret = 0;
	int state = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (rx_channel >= 0) &&
		     (rx_channel < LAN743X_NUMBER_OF_RX_CHANNELS));
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     dmac->flags & DMAC_FLAG_RX_USED(rx_channel));

	state = lan743x_dmac_rx_get_state(adapter, rx_channel);
	if (state == DMAC_CHANNEL_STATE_STARTED) {
		lan743x_csr_write(adapter, DMAC_CMD,
				  DMAC_CMD_STOP_R_(rx_channel));
		state = lan743x_dmac_rx_wait_till_stopped(adapter, rx_channel);
		if (state < 0) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "failed wait till not stop pending, rx_channel = %d",
				    rx_channel);
			ret = -ENODEV;
		}
	} else if (state == DMAC_CHANNEL_STATE_STOP_PENDING) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "A stop is already pending for rx_channel = %d",
			    rx_channel);
		state = lan743x_dmac_rx_wait_till_stopped(adapter, rx_channel);
		if (state < 0) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "failed wait till not stop pending, rx_channel = %d",
				    rx_channel);
			ret = -ENODEV;
		}
	} else {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "Attempting to stop a not started rx channel = %d",
			      rx_channel);
	}
	return ret;
}

/* TX */

/* TX Descriptor bits */
#define TX_DESC_DATA0_DTYPE_MASK_     (0xC0000000)
#define TX_DESC_DATA0_DTYPE_DATA_     (0x00000000)
#define TX_DESC_DATA0_DTYPE_EXT_      (0x40000000)
#define TX_DESC_DATA0_FS_             (0x20000000)
#define TX_DESC_DATA0_LS_             (0x10000000)
#define TX_DESC_DATA0_EXT_            (0x08000000)
#define TX_DESC_DATA0_IOC_            (0x04000000)
#define TX_DESC_DATA0_DTI_            (0x02000000)
#define TX_DESC_DATA0_TSI_            (0x01000000)
#define TX_DESC_DATA0_IGE_            (0x00800000)
#define TX_DESC_DATA0_ICE_            (0x00400000)
#define TX_DESC_DATA0_IPE_            (0x00200000)
#define TX_DESC_DATA0_TPE_            (0x00100000)
#define TX_DESC_DATA0_IVTG_           (0x00080000)
#define TX_DESC_DATA0_RVTG_           (0x00040000)
#define TX_DESC_DATA0_FCS_            (0x00020000)
#define TX_DESC_DATA0_TSE_            (0x00010000)
#define TX_DESC_DATA0_BUF_LENGTH_MASK_        (0x0000FFFF)

#define TX_DESC_DATA0_EXT_LSO_            (0x00200000)
#define TX_DESC_DATA0_EXT_PAY_LENGTH_MASK_    (0x000FFFFF)

#define TX_DESC_DATA1_TADDRL_MASK_        (0xFFFFFFFF)
#define TX_DESC_DATA2_TADDRH_MASK_        (0xFFFFFFFF)

#define TX_DESC_DATA3_FRAME_LENGTH_MSS_MASK_  (0x3FFF0000)
#define TX_DESC_DATA3_VTAG_MASK_          (0x0000FFFF)

struct lan743x_tx_descriptor {
	u32     data0;
	u32     data1;
	u32     data2;
	u32     data3;
} __aligned(DEFAULT_DMA_DESCRIPTOR_SPACING);

#define TX_BUFFER_INFO_FLAG_ACTIVE                  BIT(0)
#define TX_BUFFER_INFO_FLAG_TIMESTAMP_REQUESTED     BIT(1)
#define TX_BUFFER_INFO_FLAG_SKB_FRAGMENT            BIT(2)
struct lan743x_tx_buffer_info {
	int flags;
	struct sk_buff *skb;
	dma_addr_t      dma_ptr;
	unsigned int    buffer_length;
};

#define LAN743X_TX_RING_SIZE    (50)

static void lan743x_tx_isr(void *context, u32 int_sts)
{
	int enable_flag = 1;
	struct lan743x_tx *tx = (struct lan743x_tx *)context;
	struct lan743x_adapter *adapter = tx->adapter;

	lan743x_csr_write(adapter, INT_EN_CLR,
			  INT_BIT_DMA_TX_(tx->channel_number));

	if (int_sts & INT_BIT_DMA_TX_(tx->channel_number)) {
		u32 dmac_int_sts = lan743x_csr_read(adapter, DMAC_INT_STS);
		u32 dmac_int_en = lan743x_csr_read(adapter, DMAC_INT_EN_SET);
		u32 ioc_bit = DMAC_INT_BIT_TX_IOC_(tx->channel_number);
		u32 stop_bit = DMAC_INT_BIT_TX_STOP_(tx->channel_number);

		dmac_int_en &= (ioc_bit | stop_bit);
		dmac_int_sts &= dmac_int_en;

		if (dmac_int_sts & ioc_bit) {
			tasklet_schedule(&tx->tx_isr_bottom_half);
			enable_flag = 0;/* tasklet will re-enable later */
		}
		if (dmac_int_sts & stop_bit) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "unhandled tx stop interrupt");
			/* clear dmac int sts */
			lan743x_csr_write(adapter, DMAC_INT_STS, stop_bit);
		}
	} else {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "unexpected interrupt, INT_BIT_DMA_TX_(%d) == 0",
			    tx->channel_number);
	}
	if (enable_flag)
		/* enable isr */
		lan743x_csr_write(adapter, INT_EN_SET,
				  INT_BIT_DMA_TX_(tx->channel_number));
}

static void lan743x_tx_release_desc(struct lan743x_tx *tx,
				    int descriptor_index, bool cleanup)
{
	struct lan743x_adapter *adapter = tx->adapter;
	struct lan743x_tx_descriptor *descriptor = NULL;
	struct lan743x_tx_buffer_info *buffer_info = NULL;
	u32 descriptor_type = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (descriptor_index >= 0) &&
		     (descriptor_index < tx->ring_size));

	descriptor = &tx->ring_cpu_ptr[descriptor_index];
	buffer_info = &tx->buffer_info[descriptor_index];
	if (!(buffer_info->flags & TX_BUFFER_INFO_FLAG_ACTIVE)) {
		NETIF_ASSERT(adapter, drv, adapter->netdev, !buffer_info->skb);
		NETIF_ASSERT(adapter, drv, adapter->netdev,
			     !buffer_info->dma_ptr);
		NETIF_ASSERT(adapter, drv, adapter->netdev,
			     !buffer_info->buffer_length);
		goto done;
	}

	descriptor_type = (descriptor->data0) &
			  TX_DESC_DATA0_DTYPE_MASK_;
	if (descriptor_type == TX_DESC_DATA0_DTYPE_DATA_) {
		goto clean_up_data_descriptor;
	} else if (descriptor_type == TX_DESC_DATA0_DTYPE_EXT_) {
		/* ignore extension type */
		NETIF_ASSERT(adapter, drv, adapter->netdev, !buffer_info->skb);
		NETIF_ASSERT(adapter, drv, adapter->netdev,
			     !buffer_info->dma_ptr);
		NETIF_ASSERT(adapter, drv, adapter->netdev,
			     !buffer_info->buffer_length);
		goto clear_active;
	} else {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Unexpected descriptor type");
		goto clear_active;
	}
clean_up_data_descriptor:
	if (buffer_info->dma_ptr) {
		if (buffer_info->flags &
		    TX_BUFFER_INFO_FLAG_SKB_FRAGMENT) {
			NETIF_ASSERT(adapter, drv, adapter->netdev,
				     !(descriptor->data0 &
				     TX_DESC_DATA0_FS_));
			dma_unmap_page(&tx->adapter->pci.pdev->dev,
				       buffer_info->dma_ptr,
				       buffer_info->buffer_length,
				       DMA_TO_DEVICE);
		} else {
			NETIF_ASSERT(adapter, drv, adapter->netdev,
				     (descriptor->data0 &
				     TX_DESC_DATA0_FS_));
			dma_unmap_single(&tx->adapter->pci.pdev->dev,
					 buffer_info->dma_ptr,
					 buffer_info->buffer_length,
					 DMA_TO_DEVICE);
		}
		buffer_info->dma_ptr = 0;
		buffer_info->buffer_length = 0;
	} else {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "buffer_info->phys_ptr == NULL at %d",
			    descriptor_index);
	}
	if (buffer_info->skb) {
		NETIF_ASSERT(adapter, drv, adapter->netdev,
			     (descriptor->data0 &
			     TX_DESC_DATA0_LS_));
		if (buffer_info->flags &
		    TX_BUFFER_INFO_FLAG_TIMESTAMP_REQUESTED) {
			if (cleanup) {
				lan743x_ptp_unrequest_tx_timestamp(tx->adapter);
				dev_kfree_skb(buffer_info->skb);
			} else {
				lan743x_ptp_tx_timestamp_skb(tx->adapter,
							     buffer_info->skb);
			}
		} else {
			dev_kfree_skb(buffer_info->skb);
		}
		buffer_info->skb = NULL;
	}
clear_active:
	buffer_info->flags &= ~TX_BUFFER_INFO_FLAG_ACTIVE;
done:
	memset(buffer_info, 0, sizeof(*buffer_info));
	memset(descriptor, 0, sizeof(*descriptor));
}

static inline int lan743x_tx_next_index(struct lan743x_tx *tx, int index)
{
	return ((++index) % tx->ring_size);
}

static void lan743x_tx_release_completed_descriptors(struct lan743x_tx *tx)
{
	while ((*tx->head_cpu_ptr) != (tx->last_head)) {
		lan743x_tx_release_desc(tx, tx->last_head, false);

		tx->last_head = lan743x_tx_next_index(tx, tx->last_head);
	}
}

static void lan743x_tx_release_all_descriptors(struct lan743x_tx *tx)
{
	u32 original_head = 0;

	original_head = tx->last_head;

	do {
		lan743x_tx_release_desc(tx, tx->last_head, true);
		tx->last_head = lan743x_tx_next_index(tx, tx->last_head);
	} while (tx->last_head != original_head);

	memset(tx->ring_cpu_ptr, 0,
	       sizeof(*tx->ring_cpu_ptr) * (tx->ring_size));
	memset(tx->buffer_info, 0,
	       sizeof(*tx->buffer_info) * (tx->ring_size));
}

static int lan743x_tx_get_desc_cnt(struct lan743x_tx *tx,
				   struct sk_buff *skb)
{
	struct lan743x_adapter *adapter = tx->adapter;
	int result = 1;/* 1 for the main skb buffer */
	int nr_frags = 0;

	NETIF_ASSERT(adapter, drv, adapter->netdev, skb);
	if (skb_is_gso(skb))
		result++;/* requires an extension descriptor */
	nr_frags = skb_shinfo(skb)->nr_frags;
	NETIF_ASSERT(adapter, drv, adapter->netdev, nr_frags >= 0);
	result += nr_frags; /* 1 for each fragment buffer */
	return result;
}

static int lan743x_tx_get_avail_desc(struct lan743x_tx *tx)
{
	struct lan743x_adapter *adapter = tx->adapter;
	int last_head = tx->last_head;
	int last_tail = tx->last_tail;

	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     (last_tail >= 0) && (last_tail < tx->ring_size));
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     (last_head >= 0) && (last_head < tx->ring_size));
	if (last_tail >= last_head)
		return tx->ring_size - last_tail + last_head - 1;
	else
		return last_head - last_tail - 1;
}

static void lan743x_tx_isr_bottom_half(unsigned long param)
{
	unsigned long irq_flags = 0;
	struct lan743x_tx *tx = NULL;
	struct lan743x_adapter *adapter = NULL;
	bool start_transmitter = false;
	u32 ioc_bit = 0;

	tx = (struct lan743x_tx *)param;
	adapter = tx->adapter;
	NETIF_ASSERT(adapter, drv, adapter->netdev, tx->ring_cpu_ptr);
	ioc_bit = DMAC_INT_BIT_TX_IOC_(tx->channel_number);

	spin_lock_irqsave(&tx->ring_lock, irq_flags);
	do {
		/* clear dmac int sts */
		lan743x_csr_write(adapter, DMAC_INT_STS, ioc_bit);
		lan743x_csr_read(adapter, DMAC_INT_STS);

		/* clean up tx ring */
		lan743x_tx_release_completed_descriptors(tx);
	} while (lan743x_csr_read(adapter, DMAC_INT_STS) & ioc_bit);

	if (netif_queue_stopped(tx->adapter->netdev)) {
		if (tx->overflow_skb) {
			if (lan743x_tx_get_desc_cnt(tx, tx->overflow_skb) <=
						    lan743x_tx_get_avail_desc(
						    tx))
				start_transmitter = true;
		} else {
			NETIF_WARNING(adapter, drv, adapter->netdev,
				      "Why was queue stopped, with out any overflow skb?");
			netif_wake_queue(tx->adapter->netdev);
		}
	} else if (tx->overflow_skb) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "why is queue not stopped when overflow skb is used");
	}
	spin_unlock_irqrestore(&tx->ring_lock, irq_flags);

	if (start_transmitter) {
		/* space is now available, transmit overflow skb */
		lan743x_tx_xmit_frame(tx, tx->overflow_skb);
		tx->overflow_skb = NULL;
		netif_wake_queue(adapter->netdev);
	}

	/* enable isr */
	lan743x_csr_write(adapter, INT_EN_SET,
			  INT_BIT_DMA_TX_(tx->channel_number));
	lan743x_csr_read(adapter, INT_STS);
}

static int lan743x_tx_ring_init(struct lan743x_tx *tx)
{
	int ret = -ENOMEM;
	int descriptor_spacing = 0;
	size_t ring_allocation_size = 0;
	dma_addr_t dma_ptr;
	void *cpu_ptr = NULL;
	struct lan743x_adapter *adapter = tx->adapter;

	NETIF_ASSERT(adapter, drv, adapter->netdev, tx->adapter);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !tx->ring_size);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !tx->ring_allocation_size);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !tx->ring_cpu_ptr);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !tx->ring_dma_ptr);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !tx->buffer_info);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !tx->head_cpu_ptr);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !tx->head_dma_ptr);

	descriptor_spacing = lan743x_dmac_get_descriptor_spacing(tx->adapter);
	if (sizeof(struct lan743x_tx_descriptor) != descriptor_spacing)	{
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "sizeof(struct lan743x_tx_descriptor) != descriptor_spacing");
		ret = -EPERM;
		goto cleanup;
	}

	tx->ring_size = LAN743X_TX_RING_SIZE;

	if ((tx->ring_size) & (~TX_CFG_B_TX_RING_LEN_MASK_)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "ring size is too large, tx_channel = %d",
			    tx->channel_number);
		ret = -EINVAL;
		goto cleanup;
	}

	ring_allocation_size = ALIGN(tx->ring_size * descriptor_spacing,
				     PAGE_SIZE);

	dma_ptr = 0;
	cpu_ptr = pci_zalloc_consistent(tx->adapter->pci.pdev,
					ring_allocation_size, &dma_ptr);
	if (!cpu_ptr) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Failed to allocate tx ring, channel = %d",
			    tx->channel_number);
		ret = -ENOMEM;
		goto cleanup;
	}
	NETIF_ASSERT(adapter, drv, adapter->netdev, dma_ptr);
	tx->ring_allocation_size = ring_allocation_size;
	tx->ring_cpu_ptr = (struct lan743x_tx_descriptor *)cpu_ptr;
	tx->ring_dma_ptr = dma_ptr;
	if ((tx->ring_dma_ptr) & 0x3) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "dma ring base is not DWORD aligned, channel = %d",
			    tx->channel_number);
		ret = -ENOMEM;
		goto cleanup;
	}

	cpu_ptr = kzalloc(tx->ring_size * sizeof(*tx->buffer_info), GFP_KERNEL);
	if (!cpu_ptr) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Failed to allocate buffer info, channel = %d",
			    tx->channel_number);
		ret = -ENOMEM;
		goto cleanup;
	}
	tx->buffer_info = (struct lan743x_tx_buffer_info *)cpu_ptr;

	dma_ptr = 0;
	cpu_ptr = pci_zalloc_consistent(tx->adapter->pci.pdev,
					sizeof(*tx->head_cpu_ptr), &dma_ptr);
	if (!cpu_ptr) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Failed to allocate head pointer, channel = %d",
			    tx->channel_number);
		ret = -ENOMEM;
		goto cleanup;
	}
	NETIF_ASSERT(adapter, drv, adapter->netdev, dma_ptr);
	tx->head_cpu_ptr = cpu_ptr;
	tx->head_dma_ptr = dma_ptr;
	if ((tx->head_dma_ptr) & 0x3) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "head write back pointer is not DWORD aligned, channel = %d",
			    tx->channel_number);
		ret = -ENOMEM;
		goto cleanup;
	}

	ret = 0;
cleanup:
	if (ret)
		lan743x_tx_ring_cleanup(tx);
	return ret;
}

static void lan743x_tx_ring_cleanup(struct lan743x_tx *tx)
{
	if (tx->head_cpu_ptr) {
		pci_free_consistent(tx->adapter->pci.pdev,
				    sizeof(*tx->head_cpu_ptr),
				    (void *)(tx->head_cpu_ptr),
				    tx->head_dma_ptr);
		tx->head_cpu_ptr = NULL;
		tx->head_dma_ptr = 0;
	}

	kfree(tx->buffer_info);
	tx->buffer_info = NULL;

	if (tx->ring_cpu_ptr) {
		pci_free_consistent(tx->adapter->pci.pdev,
				    tx->ring_allocation_size,
				    tx->ring_cpu_ptr,
				    tx->ring_dma_ptr);
		tx->ring_allocation_size = 0;
		tx->ring_cpu_ptr = NULL;
		tx->ring_dma_ptr = 0;
	}

	tx->ring_size = 0;
}

static int lan743x_tx_init(struct lan743x_tx *tx,
			   struct lan743x_adapter *adapter, int tx_channel)
{
	int ret = -ENODEV;

	NETIF_ASSERT(adapter, probe, adapter->netdev, tx);
	NETIF_ASSERT(adapter, probe, adapter->netdev, adapter);
	memset(tx, 0, sizeof(*tx));
	NETIF_ASSERT(adapter, probe, adapter->netdev, (tx_channel >= 0) &&
		     (tx_channel < LAN743X_NUMBER_OF_TX_CHANNELS));

	tx->adapter = adapter;
	tx->channel_number = -1;

	ret = lan743x_dmac_reserve_tx_channel(adapter, tx_channel);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "Failed to reserve tx channel %d", tx_channel);
		goto cleanup;
	}
	tx->channel_number = tx_channel;

	spin_lock_init(&tx->ring_lock);

	tasklet_init(&tx->tx_isr_bottom_half,
		     lan743x_tx_isr_bottom_half, (unsigned long)tx);
	tasklet_disable(&tx->tx_isr_bottom_half);

	ret = 0;

cleanup:
	if (ret)
		lan743x_tx_cleanup(tx);
	return ret;
}

static void lan743x_tx_cleanup(struct lan743x_tx *tx)
{
	struct lan743x_adapter *adapter = tx->adapter;

	if (tx->channel_number >= 0) {
		lan743x_dmac_release_tx_channel(adapter, tx->channel_number);
		tx->channel_number = -1;
	}

	memset(tx, 0, sizeof(*tx));
}

static int lan743x_tx_open(struct lan743x_tx *tx)
{
	int ret = -ENODEV;
	struct lan743x_adapter *adapter = NULL;
	u32 data = 0;

	NETIF_ASSERT(adapter, ifup, adapter->netdev,
		     (tx->channel_number >= 0) &&
		     (tx->channel_number < LAN743X_NUMBER_OF_TX_CHANNELS));
	adapter = tx->adapter;

	ret = lan743x_tx_ring_init(tx);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "Tx Channel = %d, failed to initialize dma ring",
			    tx->channel_number);
		ret = -ENOMEM;
		goto cleanup;
	}
	tx->flags |= TX_FLAG_RING_ALLOCATED;

	/* enable mac */
	ret = lan743x_mac_tx_enable(adapter, tx->channel_number);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "failed to enable mac, tx_channel = %d",
			    tx->channel_number);
		goto cleanup;
	}
	tx->flags |= TX_FLAG_MAC_ENABLED;

	/* initialize fifo */
	NETIF_ASSERT(adapter, ifup, adapter->netdev,
		     !(tx->flags & TX_FLAG_FIFO_ENABLED));
	ret = lan743x_fct_tx_reset(adapter, tx->channel_number);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "Failed to reset tx fifo, tx_channel = %d",
			    tx->channel_number);
		goto cleanup;
	}

	/* enable fifo */
	ret = lan743x_fct_tx_enable(adapter, tx->channel_number);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "Failed to enable tx fifo, tx_channel = %d",
			    tx->channel_number);
		goto cleanup;
	}
	tx->flags |= TX_FLAG_FIFO_ENABLED;

	/* reset tx channel */
	ret = lan743x_dmac_tx_reset(adapter, tx->channel_number);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "Failed to reset tx dmac, tx_channel = %d",
			    tx->channel_number);
		goto cleanup;
	}

	/* Write TX_BASE_ADDR */
	NETIF_ASSERT(adapter, ifup, adapter->netdev,
		     !((tx->ring_dma_ptr) & 0x3));
	lan743x_csr_write(adapter,
			  TX_BASE_ADDRH(tx->channel_number),
			  DMA_ADDR_HIGH32(tx->ring_dma_ptr));
	lan743x_csr_write(adapter,
			  TX_BASE_ADDRL(tx->channel_number),
			  DMA_ADDR_LOW32(tx->ring_dma_ptr));

	/* Write TX_CFG_B */
	NETIF_ASSERT(adapter, ifup, adapter->netdev,
		     !((tx->ring_size) & (~TX_CFG_B_TX_RING_LEN_MASK_)));
	data = lan743x_csr_read(adapter, TX_CFG_B(tx->channel_number));
	data &= ~TX_CFG_B_TX_RING_LEN_MASK_;
	data |= ((tx->ring_size) & TX_CFG_B_TX_RING_LEN_MASK_);
	lan743x_csr_write(adapter, TX_CFG_B(tx->channel_number), data);

	/* Write TX_CFG_A */
	data = TX_CFG_A_TX_TMR_HPWB_SEL_IOC_ | TX_CFG_A_TX_HP_WB_EN_;
	lan743x_csr_write(adapter, TX_CFG_A(tx->channel_number), data);

	/* Write TX_HEAD_WRITEBACK_ADDR */
	NETIF_ASSERT(adapter, ifup, adapter->netdev,
		     !((tx->head_dma_ptr) & 0x3));
	lan743x_csr_write(adapter,
			  TX_HEAD_WRITEBACK_ADDRH(tx->channel_number),
			  DMA_ADDR_HIGH32(tx->head_dma_ptr));
	lan743x_csr_write(adapter,
			  TX_HEAD_WRITEBACK_ADDRL(tx->channel_number),
			  DMA_ADDR_LOW32(tx->head_dma_ptr));

	/* set last head */
	tx->last_head = lan743x_csr_read(adapter, TX_HEAD(tx->channel_number));
	NETIF_ASSERT(adapter, ifup, adapter->netdev, !tx->last_head);

	/* write TX_TAIL */
	tx->last_tail = 0;
	lan743x_csr_write(adapter, TX_TAIL(tx->channel_number),
			  (u32)(tx->last_tail));

	tasklet_enable(&tx->tx_isr_bottom_half);
	lan743x_csr_write(adapter, INT_EN_SET,
			  INT_BIT_DMA_TX_(tx->channel_number));
	lan743x_csr_write(adapter, DMAC_INT_EN_SET,
			  DMAC_INT_BIT_TX_IOC_(tx->channel_number));
	tx->flags |= TX_FLAG_ISR_ENABLED;

	/*  start dmac channel */
	ret = lan743x_dmac_tx_start(adapter, tx->channel_number);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "Failed to start tx channel %d",
			    tx->channel_number);
		goto cleanup;
	}
	tx->flags |= TX_FLAG_DMAC_STARTED;

	ret = 0;

cleanup:
	if (ret)
		lan743x_tx_close(tx);
	return ret;
}

static void lan743x_tx_close(struct lan743x_tx *tx)
{
	struct lan743x_adapter *adapter = tx->adapter;

	if (tx->flags & TX_FLAG_DMAC_STARTED) {
		lan743x_dmac_tx_stop(adapter, tx->channel_number);
		tx->flags &= ~TX_FLAG_DMAC_STARTED;
	}
	if (tx->flags & TX_FLAG_ISR_ENABLED) {
		lan743x_csr_write(adapter,
				  DMAC_INT_EN_CLR,
				  DMAC_INT_BIT_TX_IOC_(tx->channel_number));
		lan743x_csr_write(adapter, INT_EN_CLR,
				  INT_BIT_DMA_TX_(tx->channel_number));
		tasklet_disable(&tx->tx_isr_bottom_half);
		tx->flags &= ~TX_FLAG_ISR_ENABLED;
	}

	if (tx->flags & TX_FLAG_FIFO_ENABLED) {
		lan743x_fct_tx_disable(adapter, tx->channel_number);
		tx->flags &= ~TX_FLAG_FIFO_ENABLED;
	}

	if (tx->flags & TX_FLAG_MAC_ENABLED) {
		lan743x_mac_tx_disable(adapter, tx->channel_number);
		tx->flags &= ~TX_FLAG_MAC_ENABLED;
	}

	lan743x_tx_release_all_descriptors(tx);

	if (tx->overflow_skb) {
		dev_kfree_skb(tx->overflow_skb);
		tx->overflow_skb = NULL;
	}

	if (tx->flags & TX_FLAG_RING_ALLOCATED) {
		lan743x_tx_ring_cleanup(tx);
		tx->flags &= ~TX_FLAG_RING_ALLOCATED;
	}
}

static void lan743x_tx_set_timestamping_enable(struct lan743x_tx *tx,
					       bool enabled)
{
	if (enabled)
		tx->flags |= TX_FLAG_TIMESTAMPING_ENABLED;
	else
		tx->flags &= ~TX_FLAG_TIMESTAMPING_ENABLED;
}

static int lan743x_tx_frame_start(struct lan743x_tx *tx,
				  unsigned char *first_buffer,
				  unsigned int first_buffer_length,
				  unsigned int frame_length,
				  bool time_stamp,
				  bool check_sum)
{
	/* called only from within lan743x_tx_xmit_frame.
	 * assuming tx->ring_lock has already been acquired.
	 */
	struct lan743x_adapter *adapter = tx->adapter;
	struct lan743x_tx_descriptor *tx_descriptor = NULL;
	struct lan743x_tx_buffer_info *buffer_info = NULL;
	struct device *dev = &adapter->pci.pdev->dev;
	dma_addr_t dma_ptr;

	NETIF_ASSERT(adapter, tx_queued, adapter->netdev, first_buffer);
	NETIF_ASSERT(adapter, tx_queued, adapter->netdev,
		     !(tx->frame_flags & TX_FRAME_FLAG_IN_PROGRESS));

	tx->frame_flags |= TX_FRAME_FLAG_IN_PROGRESS;

	tx->frame_first = lan743x_csr_read(adapter,
					   TX_TAIL(tx->channel_number));
	tx->frame_tail = tx->frame_first;

	if (tx->frame_tail != tx->last_tail) {
		NETIF_ERROR(adapter, tx_queued, adapter->netdev,
			    "unexpected tail index, tail=%d, last_tail=%d",
			    tx->frame_tail, tx->last_tail);
		return -EPERM;
	}

	tx_descriptor = &tx->ring_cpu_ptr[tx->frame_tail];
	buffer_info = &tx->buffer_info[tx->frame_tail];

	dma_ptr = dma_map_single(dev, first_buffer, first_buffer_length,
				 DMA_TO_DEVICE);
	if (dma_mapping_error(dev, dma_ptr)) {
		NETIF_ERROR(adapter, tx_queued, adapter->netdev,
			    "DMA mapping error");
		return -ENOMEM;
	}

	tx_descriptor->data1 = DMA_ADDR_LOW32(dma_ptr);
	tx_descriptor->data2 = DMA_ADDR_HIGH32(dma_ptr);
	tx_descriptor->data3 = (frame_length << 16) &
			       TX_DESC_DATA3_FRAME_LENGTH_MSS_MASK_;

	NETIF_ASSERT(adapter, tx_queued, adapter->netdev,
		     !((buffer_info->flags) & TX_BUFFER_INFO_FLAG_ACTIVE));
	NETIF_ASSERT(adapter, tx_queued, adapter->netdev, !buffer_info->skb);

	buffer_info->skb = NULL;
	buffer_info->dma_ptr = dma_ptr;
	buffer_info->buffer_length = first_buffer_length;
	buffer_info->flags |= TX_BUFFER_INFO_FLAG_ACTIVE;

	tx->frame_data0 = (first_buffer_length &
			  TX_DESC_DATA0_BUF_LENGTH_MASK_) |
			  TX_DESC_DATA0_DTYPE_DATA_ |
			  TX_DESC_DATA0_FS_ |
			  TX_DESC_DATA0_FCS_;

	if (time_stamp)
		tx->frame_data0 |= TX_DESC_DATA0_TSE_;
	if (check_sum)
		tx->frame_data0 |= TX_DESC_DATA0_ICE_ |
				   TX_DESC_DATA0_IPE_ |
				   TX_DESC_DATA0_TPE_;

	/* data0 will be programmed in one of other frame assembler functions */

	return 0;
}

static void lan743x_tx_frame_add_lso(struct lan743x_tx *tx,
				     unsigned int frame_length)
{
	/* called only from within lan743x_tx_xmit_frame.
	 * assuming tx->ring_lock has already been acquired.
	 */
	struct lan743x_adapter *adapter = tx->adapter;
	struct lan743x_tx_descriptor *tx_descriptor = NULL;
	struct lan743x_tx_buffer_info *buffer_info = NULL;

	NETIF_ASSERT(adapter, tx_queued, adapter->netdev,
		     (tx->frame_flags & TX_FRAME_FLAG_IN_PROGRESS));

	/* wrap up previous descriptor */
	tx->frame_data0 |= TX_DESC_DATA0_EXT_;
	tx_descriptor = &tx->ring_cpu_ptr[tx->frame_tail];
	tx_descriptor->data0 = tx->frame_data0;

	/* move to next descriptor */
	tx->frame_tail = lan743x_tx_next_index(tx, tx->frame_tail);
	tx_descriptor = &tx->ring_cpu_ptr[tx->frame_tail];
	buffer_info = &tx->buffer_info[tx->frame_tail];

	/* add extension descriptor */
	tx_descriptor->data1 = 0;
	tx_descriptor->data2 = 0;
	tx_descriptor->data3 = 0;

	buffer_info->skb = NULL;
	buffer_info->dma_ptr = 0;
	buffer_info->buffer_length = 0;
	buffer_info->flags |= TX_BUFFER_INFO_FLAG_ACTIVE;

	tx->frame_data0 = (frame_length & TX_DESC_DATA0_EXT_PAY_LENGTH_MASK_) |
			  TX_DESC_DATA0_DTYPE_EXT_ |
			  TX_DESC_DATA0_EXT_LSO_;

	/* data0 will be programmed in one of other frame assembler functions */
}

static int lan743x_tx_frame_add_fragment(struct lan743x_tx *tx,
					 const struct skb_frag_struct *fragment,
					 unsigned int frame_length)
{
	/* called only from within lan743x_tx_xmit_frame
	 * assuming tx->ring_lock has already been acquired
	 */
	struct lan743x_adapter *adapter = tx->adapter;
	struct lan743x_tx_descriptor *tx_descriptor = NULL;
	struct lan743x_tx_buffer_info *buffer_info = NULL;
	unsigned int fragment_length = 0;
	struct device *dev = &adapter->pci.pdev->dev;
	dma_addr_t dma_ptr;

	NETIF_ASSERT(adapter, tx_queued, adapter->netdev,
		     (tx->frame_flags & TX_FRAME_FLAG_IN_PROGRESS));

	fragment_length = skb_frag_size(fragment);
	if (!fragment_length)
		return 0;

	/* wrap up previous descriptor */
	tx_descriptor = &tx->ring_cpu_ptr[tx->frame_tail];
	tx_descriptor->data0 = tx->frame_data0;

	/* move to next descriptor */
	tx->frame_tail = lan743x_tx_next_index(tx, tx->frame_tail);
	tx_descriptor = &tx->ring_cpu_ptr[tx->frame_tail];
	buffer_info = &tx->buffer_info[tx->frame_tail];

	dma_ptr = skb_frag_dma_map(dev, fragment,
				   0, fragment_length,
				   DMA_TO_DEVICE);
	if (dma_mapping_error(dev, dma_ptr)) {
		int desc_index;

		NETIF_ERROR(adapter, tx_queued, adapter->netdev,
			    "fragment, DMA mapping error");

		/* cleanup all previously setup descriptors */
		desc_index = tx->frame_first;
		while (desc_index != tx->frame_tail) {
			lan743x_tx_release_desc(tx, desc_index, true);
			desc_index = lan743x_tx_next_index(tx,
							   desc_index);
		}
		dma_wmb();

		tx->frame_flags &= ~TX_FRAME_FLAG_IN_PROGRESS;
		tx->frame_first = 0;
		tx->frame_data0 = 0;
		tx->frame_tail = 0;
		return -ENOMEM;
	}
	tx_descriptor->data1 = DMA_ADDR_LOW32(dma_ptr);
	tx_descriptor->data2 = DMA_ADDR_HIGH32(dma_ptr);
	tx_descriptor->data3 = (frame_length << 16) &
			       TX_DESC_DATA3_FRAME_LENGTH_MSS_MASK_;
	buffer_info->skb = NULL;
	buffer_info->dma_ptr = dma_ptr;
	buffer_info->buffer_length = fragment_length;
	buffer_info->flags |= TX_BUFFER_INFO_FLAG_ACTIVE;
	buffer_info->flags |= TX_BUFFER_INFO_FLAG_SKB_FRAGMENT;

	tx->frame_data0 = (fragment_length & TX_DESC_DATA0_BUF_LENGTH_MASK_) |
			  TX_DESC_DATA0_DTYPE_DATA_ |
			  TX_DESC_DATA0_FCS_;

	/* data0 will be programmed in one of other frame assembler functions */
	return 0;
}

static void lan743x_tx_frame_end(struct lan743x_tx *tx,
				 struct sk_buff *skb,
				 bool time_stamp)
{
	/* called only from within lan743x_tx_xmit_frame
	 * assuming tx->ring_lock has already been acquired
	 */
	struct lan743x_adapter *adapter = tx->adapter;
	struct lan743x_tx_descriptor *tx_descriptor = NULL;
	struct lan743x_tx_buffer_info *buffer_info = NULL;

	NETIF_ASSERT(adapter, tx_queued, adapter->netdev,
		     (tx->frame_flags & TX_FRAME_FLAG_IN_PROGRESS));

	/* wrap up previous descriptor */
	NETIF_ASSERT(adapter, tx_queued, adapter->netdev,
		     !(tx->frame_data0 & TX_DESC_DATA0_DTYPE_EXT_));
	tx->frame_data0 |= TX_DESC_DATA0_LS_;
	tx->frame_data0 |= TX_DESC_DATA0_IOC_;

	tx_descriptor = &tx->ring_cpu_ptr[tx->frame_tail];
	buffer_info = &tx->buffer_info[tx->frame_tail];
	buffer_info->skb = skb;
	if (time_stamp)
		buffer_info->flags |= TX_BUFFER_INFO_FLAG_TIMESTAMP_REQUESTED;
	tx_descriptor->data0 = tx->frame_data0;

	tx->frame_tail = lan743x_tx_next_index(tx, tx->frame_tail);
	tx->last_tail = tx->frame_tail;

	dma_wmb();

	lan743x_csr_write(adapter, TX_TAIL(tx->channel_number), tx->frame_tail);

	tx->frame_flags &= ~TX_FRAME_FLAG_IN_PROGRESS;
}

static netdev_tx_t lan743x_tx_xmit_frame(struct lan743x_tx *tx,
					 struct sk_buff *skb)
{
	unsigned long irq_flags = 0;
	int required_number_of_descriptors = 0;
	struct lan743x_adapter *adapter = tx->adapter;
	unsigned int frame_length = 0;
	unsigned int head_length = 0;
	unsigned int start_frame_length = 0;
	int nr_frags = 0;
	bool do_timestamp = false;
	bool gso = false;
	int j;

	NETIF_ASSERT(adapter, tx_queued, adapter->netdev, skb);

	if (skb->len > 0xFFFF) {
		NETIF_WARNING(adapter, tx_queued, adapter->netdev,
			      "dropping packet, length too large, skb->len = %d",
			      skb->len);
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&tx->ring_lock, irq_flags);

	required_number_of_descriptors = lan743x_tx_get_desc_cnt(tx, skb);
	if (required_number_of_descriptors >
		lan743x_tx_get_avail_desc(tx)) {
		if (required_number_of_descriptors > (tx->ring_size - 1)) {
			NETIF_WARNING(adapter, tx_queued, adapter->netdev,
				      "dropping packet, requires too many descriptors, %d",
				      required_number_of_descriptors);
			dev_kfree_skb(skb);
		} else {
			/* save to overflow buffer */
			NETIF_ASSERT(adapter, tx_queued, adapter->netdev,
				     !tx->overflow_skb);
			tx->overflow_skb = skb;
			netif_stop_queue(tx->adapter->netdev);
		}
		goto unlock;
	}

	/* space available, transmit skb  */

	if (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP) {
		if (tx->flags & TX_FLAG_TIMESTAMPING_ENABLED) {
			if (lan743x_ptp_request_tx_timestamp(adapter)) {
				skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
				do_timestamp = true;
			} else {
				NETIF_WARNING(adapter, tx_queued,
					      adapter->netdev,
					      "Timestamp request denied, too many requests in progress");
			}
		} else {
			NETIF_WARNING(adapter, tx_queued, adapter->netdev,
				      "Tx Timestamp requested but tx timestamping is not enabled");
		}
	}

	head_length = skb_headlen(skb);
	frame_length = skb_pagelen(skb);
	nr_frags = skb_shinfo(skb)->nr_frags;
	if (!nr_frags) {
		NETIF_ASSERT(adapter, tx_queued, adapter->netdev,
			     head_length == frame_length);
	}
	start_frame_length = frame_length;
	gso = skb_is_gso(skb);
	if (gso) {
		if (nr_frags <= 0) {
			NETIF_ERROR(adapter, tx_queued, adapter->netdev,
				    "Large segment requested, but no fragments");
			dev_kfree_skb(skb);
			goto unlock;
		}
		start_frame_length = max(skb_shinfo(skb)->gso_size,
					 (unsigned short)8);
	}

	if (lan743x_tx_frame_start(tx,
				   skb->data, head_length,
				   start_frame_length,
				   do_timestamp,
				   skb->ip_summed == CHECKSUM_PARTIAL)) {
		NETIF_ERROR(adapter, tx_queued, adapter->netdev,
			    "frame start error");
		dev_kfree_skb(skb);
		goto unlock;
	}

	if (gso) {
		NETIF_ASSERT(adapter, tx_queued, adapter->netdev,
			     nr_frags > 0);
		lan743x_tx_frame_add_lso(tx, frame_length);
	}

	if (nr_frags <= 0) {
		NETIF_ASSERT(adapter, tx_queued, adapter->netdev,
			     !nr_frags);
		goto finish;
	}

	for (j = 0; j < nr_frags; j++) {
		const struct skb_frag_struct *frag;

		frag = &(skb_shinfo(skb)->frags[j]);
		if (lan743x_tx_frame_add_fragment(tx, frag, frame_length)) {
			/* upon error no need to call
			 *	lan743x_tx_frame_end
			 * frame assembler clean up was performed inside
			 *	lan743x_tx_frame_add_fragment
			 */
			NETIF_ERROR(adapter, tx_queued, adapter->netdev,
				    "Error adding fragment to DMA ring");
			dev_kfree_skb(skb);
			goto unlock;
		}
	}

finish:
	lan743x_tx_frame_end(tx, skb, do_timestamp);
unlock:
	spin_unlock_irqrestore(&tx->ring_lock, irq_flags);
	return NETDEV_TX_OK;
}

/* RX */

/* OWN bit is set. ie, Descs are owned by RX DMAC */
#define RX_DESC_DATA0_OWN_                (0x00008000)
#define RX_DESC_DATA0_LENGTH_MASK_        (0x00003FFF)
#define RX_DESC_DATA1_RADDRL_MASK_        (0xFFFFFFFF)
#define RX_DESC_DATA2_RADDRH_MASK_        (0xFFFFFFFF)

/* OWN bit is clear. ie, Descs are owned by host */
#define RX_DESC_DATA0_FS_                 (0x80000000)
#define RX_DESC_DATA0_LS_                 (0x40000000)
#define RX_DESC_DATA0_FRAME_LENGTH_MASK_  (0x3FFF0000)
#define RX_DESC_DATA0_FRAME_LENGTH_GET_(data0) \
	(((data0) & RX_DESC_DATA0_FRAME_LENGTH_MASK_) >> 16)
#define RX_DESC_DATA0_EXT_                (0x00004000)
#define RX_DESC_DATA0_BUF_LENGTH_MASK_    (0x00003FFF)
#define RX_DESC_DATA1_RSS_TYPE_MASK_      (0xF0000000)
#define RX_DESC_DATA1_RX_STATUS_MASK_     (0x00FFFFFF)
#define RX_DESC_DATA1_RX_STATUS_PRI_      (0x00800000)
#define RX_DESC_DATA1_RX_STATUS_LEN_ERR_  (0x00400000)
#define RX_DESC_DATA1_RX_STATUS_TS_       (0x00200000)
#define RX_DESC_DATA1_RX_STATUS_1588_     (0x00100000)
#define RX_DESC_DATA1_RX_STATUS_WAKE_     (0x00080000)
#define RX_DESC_DATA1_RX_STATUS_RFE_FAIL_ (0x00040000)
#define RX_DESC_DATA1_RX_STATUS_ICE_      (0x00020000)
#define RX_DESC_DATA1_RX_STATUS_TCE_      (0x00010000)
#define RX_DESC_DATA1_RX_STATUS_IPV_      (0x00008000)
#define RX_DESC_DATA1_RX_STATUS_PID_MASK_ (0x00006000)
#define RX_DESC_DATA1_RX_STATUS_PFF_      (0x00001000)
#define RX_DESC_DATA1_RX_STATUS_BAM_      (0x00000800)
#define RX_DESC_DATA1_RX_STATUS_MAM_      (0x00000400)
#define RX_DESC_DATA1_RX_STATUS_FVTG_     (0x00000200)
#define RX_DESC_DATA1_RX_STATUS_RED_      (0x00000100)
#define RX_DESC_DATA1_RX_STATUS_RWT_      (0x00000080)
#define RX_DESC_DATA1_RX_STATUS_RUNT_     (0x00000040)
#define RX_DESC_DATA1_RX_STATUS_LONG_     (0x00000020)
#define RX_DESC_DATA1_RX_STATUS_RXE_      (0x00000010)
#define RX_DESC_DATA1_RX_STATUS_ALN_      (0x00000008)
#define RX_DESC_DATA1_RX_STATUS_FCS_      (0x00000004)
#define RX_DESC_DATA1_RX_STATUS_UAM_      (0x00000002)
#define RX_DESC_DATA1_RX_STATUS_ICSM_     (0x00000001)

#define RX_DESC_DATA2_CSUM_MASK_          (0xFFFF0000)
#define RX_DESC_DATA2_VTAG_MASK_          (0x0000FFFF)
#define RX_DESC_DATA2_TS_NS_MASK_         (0x3FFFFFFF)

#define RX_DESC_DATA3_RSSHASH_MASK_       (0xFFFFFFFF)

#if ((NET_IP_ALIGN != 0) && (NET_IP_ALIGN != 2))
#error NET_IP_ALIGN must be 0 or 2
#endif

#define RX_HEAD_PADDING		NET_IP_ALIGN

struct lan743x_rx_descriptor {
	u32     data0;
	u32     data1;
	u32     data2;
	u32     data3;
} __aligned(DEFAULT_DMA_DESCRIPTOR_SPACING);

#define RX_BUFFER_INFO_FLAG_ACTIVE      BIT(0)
struct lan743x_rx_buffer_info {
	int flags;
	struct sk_buff *skb;

	dma_addr_t      dma_ptr;
	unsigned int    buffer_length;
};

#define LAN743X_RX_RING_SIZE        (65)

static inline int lan743x_rx_next_index(struct lan743x_rx *rx, int index)
{
	return ((++index) % rx->ring_size);
}

static int lan743x_rx_allocate_ring_element(struct lan743x_rx *rx,
					    int element_index)
{
	int ret = 0;
	struct lan743x_rx_descriptor *descriptor;
	struct lan743x_rx_buffer_info *buffer_info;
	int length = 0;
	struct lan743x_adapter *adapter = rx->adapter;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (element_index >= 0) &&
		     (element_index < rx->ring_size));
	NETIF_ASSERT(adapter, drv, adapter->netdev, rx->ring_cpu_ptr);
	NETIF_ASSERT(adapter, drv, adapter->netdev, rx->buffer_info);
	length = (LAN743X_MAX_FRAME_SIZE + ETH_HLEN + 4 + RX_HEAD_PADDING);
	descriptor = &rx->ring_cpu_ptr[element_index];
	buffer_info = &rx->buffer_info[element_index];

	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     !(descriptor->data0 & RX_DESC_DATA0_OWN_));
	NETIF_ASSERT(adapter, drv, adapter->netdev, !buffer_info->skb);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !buffer_info->dma_ptr);
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     !buffer_info->buffer_length);

	buffer_info->skb = __netdev_alloc_skb(rx->adapter->netdev,
					      length,
					      GFP_ATOMIC | GFP_DMA);
	if (!(buffer_info->skb)) {
		ret = -ENOMEM;
		goto done;
	}

	buffer_info->dma_ptr = dma_map_single(&rx->adapter->pci.pdev->dev,
					      buffer_info->skb->data,
					      length,
					      DMA_FROM_DEVICE);
	if (dma_mapping_error(&rx->adapter->pci.pdev->dev,
			      buffer_info->dma_ptr)) {
		buffer_info->dma_ptr = 0;
		ret = -ENOMEM;
		goto done;
	}
	buffer_info->buffer_length = length;

	descriptor->data1 = DMA_ADDR_LOW32(buffer_info->dma_ptr);
	descriptor->data2 = DMA_ADDR_HIGH32(buffer_info->dma_ptr);
	descriptor->data3 = 0;
	descriptor->data0 = (RX_DESC_DATA0_OWN_ |
			    (length & RX_DESC_DATA0_BUF_LENGTH_MASK_));

	skb_reserve(buffer_info->skb, RX_HEAD_PADDING);

	ret = 0;
done:
	return ret;
}

static void lan743x_rx_reuse_ring_element(struct lan743x_rx *rx,
					  int element_index)
{
	struct lan743x_rx_descriptor *descriptor;
	struct lan743x_rx_buffer_info *buffer_info;
	struct lan743x_adapter *adapter = rx->adapter;

	descriptor = &rx->ring_cpu_ptr[element_index];
	buffer_info = &rx->buffer_info[element_index];
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     !(descriptor->data0 & RX_DESC_DATA0_OWN_));
	NETIF_ASSERT(adapter, drv, adapter->netdev, buffer_info->skb);
	NETIF_ASSERT(adapter, drv, adapter->netdev, buffer_info->dma_ptr);
	NETIF_ASSERT(adapter, drv, adapter->netdev,
		     buffer_info->buffer_length);

	descriptor->data1 = DMA_ADDR_LOW32(buffer_info->dma_ptr);
	descriptor->data2 = DMA_ADDR_HIGH32(buffer_info->dma_ptr);
	descriptor->data3 = 0;
	descriptor->data0 = (RX_DESC_DATA0_OWN_ |
			    ((buffer_info->buffer_length) &
			    RX_DESC_DATA0_BUF_LENGTH_MASK_));
}

static void lan743x_rx_release_ring_element(struct lan743x_rx *rx,
					    int element_index)
{
	struct lan743x_rx_descriptor *descriptor;
	struct lan743x_rx_buffer_info *buffer_info;
	struct lan743x_adapter *adapter = rx->adapter;

	NETIF_ASSERT(adapter, drv, adapter->netdev, (element_index >= 0) &&
		     (element_index < rx->ring_size));
	descriptor = &rx->ring_cpu_ptr[element_index];
	buffer_info = &rx->buffer_info[element_index];
	memset(descriptor, 0, sizeof(*descriptor));
	if (buffer_info->dma_ptr) {
		dma_unmap_single(&rx->adapter->pci.pdev->dev,
				 buffer_info->dma_ptr,
				 buffer_info->buffer_length,
				 DMA_FROM_DEVICE);
		buffer_info->dma_ptr = 0;
	}
	if (buffer_info->skb) {
		dev_kfree_skb(buffer_info->skb);
		buffer_info->skb = NULL;
	}
	memset(buffer_info, 0, sizeof(*buffer_info));
}

static void lan743x_rx_isr(void *context, u32 int_sts)
{
	int enable_flag = 1;
	struct lan743x_rx *rx = (struct lan743x_rx *)context;
	struct lan743x_adapter *adapter = rx->adapter;

	lan743x_csr_write(adapter, INT_EN_CLR,
			  INT_BIT_DMA_RX_(rx->channel_number));

	if (int_sts & INT_BIT_DMA_RX_(rx->channel_number)) {
		u32 dmac_int_sts = lan743x_csr_read(adapter, DMAC_INT_STS);
		u32 dmac_int_en = lan743x_csr_read(adapter, DMAC_INT_EN_SET);
		u32 rx_frame_bit = DMAC_INT_BIT_RXFRM_(rx->channel_number);
		u32 stop_bit = DMAC_INT_BIT_RX_STOP_(rx->channel_number);

		dmac_int_en &= (rx_frame_bit | stop_bit);
		dmac_int_sts &= dmac_int_en;
		if (dmac_int_sts & rx_frame_bit) {
			napi_schedule(&rx->napi);
			enable_flag = 0;/* poll function will re-enable later */
		}
		if (dmac_int_sts & stop_bit) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "unhandled rx stop interrupt");
			/* clear dmac int sts */
			lan743x_csr_write(adapter, DMAC_INT_STS, stop_bit);
		}
	} else {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "unexpected interrupt, INT_BIT_DMA_RX_(%d) == 0",
			    rx->channel_number);
	}
	if (enable_flag) {
		/* enable isr */
		lan743x_csr_write(adapter, INT_EN_SET,
				  INT_BIT_DMA_RX_(rx->channel_number));
	}
}

#define RX_PROCESS_RESULT_NOTHING_TO_DO     (0)
#define RX_PROCESS_RESULT_PACKET_RECEIVED   (1)
#define RX_PROCESS_RESULT_PACKET_DROPPED    (2)
static int lan743x_rx_process_packet(struct lan743x_rx *rx)
{
	int result = RX_PROCESS_RESULT_NOTHING_TO_DO;
	int first_index = -1;
	int last_index = -1;
	int extension_index = -1;
	int current_head_index = -1;
	struct lan743x_rx_descriptor *descriptor;
	struct lan743x_rx_buffer_info *buffer_info;
	struct lan743x_adapter *adapter = rx->adapter;
	struct skb_shared_hwtstamps *hwtstamps = NULL;

	current_head_index = *rx->head_cpu_ptr;
	if ((current_head_index < 0) || (current_head_index >= rx->ring_size)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "out of range, current_head_index = %d",
			    current_head_index);
		goto done;
	}
	if ((rx->last_head < 0) || (rx->last_head >= rx->ring_size)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "out of range, last_head = %d",
			    rx->last_head);
		goto done;
	}
	if (rx->last_head != current_head_index) {
		descriptor = &rx->ring_cpu_ptr[rx->last_head];
		if (descriptor->data0 & RX_DESC_DATA0_OWN_) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Head index updated, but descriptor still owned by DMAC (1)");
			goto done;
		}
		if (!(descriptor->data0 & RX_DESC_DATA0_FS_)) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "first segment missing");
			goto done;
		}

		first_index = rx->last_head;
		if (descriptor->data0 & RX_DESC_DATA0_LS_) {
			last_index = rx->last_head;
		} else {
			int index;

			if (descriptor->data0 & RX_DESC_DATA0_EXT_) {
				NETIF_ERROR(adapter, drv, adapter->netdev,
					    "Extension bit set, not expected (1)");
			}
			index = lan743x_rx_next_index(rx, first_index);
			while (index != current_head_index) {
				descriptor = &rx->ring_cpu_ptr[index];
				if (descriptor->data0 & RX_DESC_DATA0_OWN_) {
					NETIF_ERROR(adapter, drv,
						    adapter->netdev,
						    "Head index updated, but descriptor still owned by DMAC (2)");
					goto done;
				}
				if (descriptor->data0 & RX_DESC_DATA0_FS_) {
					NETIF_ERROR(adapter, drv,
						    adapter->netdev,
						    "First Segment set, not expected");
				}
				if (descriptor->data0 & RX_DESC_DATA0_LS_) {
					last_index = index;
					break;
				} else if (descriptor->data0 &
					   RX_DESC_DATA0_EXT_) {
					NETIF_ERROR(adapter, drv,
						    adapter->netdev,
						    "Extension bit set, not expected (2)");
				}
				index = lan743x_rx_next_index(rx, index);
			}
		}
		if (last_index >= 0) {
			descriptor = &rx->ring_cpu_ptr[last_index];
			if (descriptor->data0 & RX_DESC_DATA0_EXT_) {
				/* extension is expected to follow */
				int index = lan743x_rx_next_index(rx,
								  last_index);
				if (index != current_head_index) {
					descriptor = &rx->ring_cpu_ptr[index];
					if (descriptor->data0 &
					    RX_DESC_DATA0_OWN_) {
						NETIF_ERROR(adapter, drv,
							    adapter->netdev,
							    "Head index updated, but descriptor still owned by DMAC (3)");
						goto done;
					}
					if (descriptor->data0 &
					    RX_DESC_DATA0_EXT_) {
						extension_index = index;
					} else {
						NETIF_ERROR(adapter, drv,
							    adapter->netdev,
							    "Expected extension after last segment");
						goto done;
					}
				} else {
					/* extension is not yet available */
					/* prevent processing of this packet */
					first_index = -1;
					last_index = -1;
				}
			}
		}
	}
	if ((first_index >= 0) && (last_index >= 0)) {
		struct sk_buff *skb = NULL;
		u32 ts_sec = 0;
		u32 ts_nsec = 0;
		int real_last_index = last_index;
		/* packet is available */
		if (first_index == last_index) {
			/* single buffer packet */
			int packet_length;

			buffer_info = &rx->buffer_info[first_index];
			NETIF_ASSERT(adapter, drv, adapter->netdev,
				     buffer_info->skb);
			skb = buffer_info->skb;
			descriptor = &rx->ring_cpu_ptr[first_index];

			/* unmap from dma */
			if (buffer_info->dma_ptr) {
				dma_unmap_single(&rx->adapter->pci.pdev->dev,
						 buffer_info->dma_ptr,
						 buffer_info->buffer_length,
						 DMA_FROM_DEVICE);
				buffer_info->dma_ptr = 0;
				buffer_info->buffer_length = 0;
			} else {
				NETIF_WARNING(adapter, drv, adapter->netdev,
					      "No DMA ptr found");
			}
			buffer_info->skb = NULL;

			packet_length =	RX_DESC_DATA0_FRAME_LENGTH_GET_(
					descriptor->data0);
			NETIF_ASSERT(adapter, drv, adapter->netdev,
				     sizeof(*skb->data) == 1);
			skb_put(skb, packet_length - 4);
			skb->protocol = eth_type_trans(skb,
						       rx->adapter->netdev);

			lan743x_rx_allocate_ring_element(rx, first_index);
		} else {
			int index = first_index;
			/* multi buffer packet */
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "multi buffer packet not supported");
			/* this should not happen since
			 * buffers are allocated to be at least jumbo size
			 */

			/* clean up buffers */
			if (first_index <= last_index) {
				while ((index >= first_index) &&
				       (index <= last_index)) {
					lan743x_rx_release_ring_element(rx,
									index);
					lan743x_rx_allocate_ring_element(rx,
									 index);
					index = lan743x_rx_next_index(rx,
								      index);
				}
			} else {
				while ((index >= first_index) ||
				       (index <= last_index)) {
					lan743x_rx_release_ring_element(rx,
									index);
					lan743x_rx_allocate_ring_element(rx,
									 index);
					index = lan743x_rx_next_index(rx,
								      index);
				}
			}
		}
		if (extension_index >= 0) {
			NETIF_ASSERT(adapter, drv, adapter->netdev,
				     extension_index ==
				     lan743x_rx_next_index(rx, last_index));
			descriptor = &rx->ring_cpu_ptr[extension_index];
			buffer_info = &rx->buffer_info[extension_index];
			NETIF_ASSERT(adapter, drv, adapter->netdev,
				     !(descriptor->data0 &
				     (RX_DESC_DATA0_FS_ |
				     RX_DESC_DATA0_LS_ |
				     RX_DESC_DATA0_OWN_)));
			NETIF_ASSERT(adapter, drv, adapter->netdev,
				     descriptor->data0 &
				     RX_DESC_DATA0_EXT_);
			ts_sec = descriptor->data1;
			ts_nsec = (descriptor->data2 &
				  RX_DESC_DATA2_TS_NS_MASK_);
			lan743x_rx_reuse_ring_element(rx, extension_index);
			real_last_index = extension_index;
		}

		if (!skb) {
			result = RX_PROCESS_RESULT_PACKET_DROPPED;
			goto move_forward;
		}
		if (extension_index < 0)
			goto pass_packet_to_os;

		hwtstamps = skb_hwtstamps(skb);
		if (hwtstamps) {
			hwtstamps->hwtstamp = ktime_set(ts_sec, ts_nsec);
		} else {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "hwtstamps == NULL");
		}
pass_packet_to_os:
		/* pass packet to OS */
		napi_gro_receive(&rx->napi, skb);
		result = RX_PROCESS_RESULT_PACKET_RECEIVED;
move_forward:
		/* push tail and head forward */
		lan743x_csr_write(adapter, RX_TAIL(rx->channel_number),
				  real_last_index);
		rx->last_head = lan743x_rx_next_index(rx, real_last_index);
	}
done:
	return result;
}

static int lan743x_rx_napi_poll(struct napi_struct *napi, int weight)
{
	int count;
	bool finished = false;
	struct lan743x_rx *rx = container_of(napi,
		struct lan743x_rx, napi);
	struct lan743x_adapter *adapter = rx->adapter;

	if (weight < 0)
		finished = true;

	count = 0;
	while (count < weight) {
		int rx_process_result = -1;

		/* clear int status bit before reading packet */
		lan743x_csr_write(adapter, DMAC_INT_STS,
				  DMAC_INT_BIT_RXFRM_(rx->channel_number));
		lan743x_csr_read(adapter, DMAC_INT_STS);

		rx_process_result = lan743x_rx_process_packet(rx);
		if (rx_process_result == RX_PROCESS_RESULT_PACKET_RECEIVED) {
			count++;
		} else if (rx_process_result ==
			RX_PROCESS_RESULT_NOTHING_TO_DO) {
			finished = true;
			break;
		} else if (rx_process_result ==
			RX_PROCESS_RESULT_PACKET_DROPPED) {
			continue;
		} else {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "Unknown rx_process_result == %d",
				    rx_process_result);
		}
	}

	adapter->netdev->stats.rx_packets += count;

	if (!finished) {
		NETIF_ASSERT(adapter, drv, adapter->netdev, count == weight);
		return count;
	}

	napi_complete_done(napi, count);

	lan743x_csr_write(adapter, INT_EN_SET,
			  INT_BIT_DMA_RX_(rx->channel_number));
	lan743x_csr_read(adapter, INT_STS);

	return 0;
}

static int lan743x_rx_ring_init(struct lan743x_rx *rx)
{
	int ret = -ENOMEM;
	int descriptor_spacing = 0;
	int element_index = 0;
	size_t ring_allocation_size = 0;
	dma_addr_t dma_ptr = 0;
	void *cpu_ptr = NULL;
	struct lan743x_adapter *adapter = rx->adapter;

	NETIF_ASSERT(adapter, drv, adapter->netdev, rx->adapter);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !rx->ring_size);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !rx->ring_allocation_size);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !rx->ring_cpu_ptr);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !rx->ring_dma_ptr);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !rx->buffer_info);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !rx->head_cpu_ptr);
	NETIF_ASSERT(adapter, drv, adapter->netdev, !rx->head_dma_ptr);

	descriptor_spacing = lan743x_dmac_get_descriptor_spacing(rx->adapter);
	if (sizeof(struct lan743x_rx_descriptor) != descriptor_spacing) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "sizeof(struct lan743x_rx_descriptor) != descriptor_spacing");
		ret = -EPERM;
		goto cleanup;
	}

	rx->ring_size = LAN743X_RX_RING_SIZE;

	if (rx->ring_size <= 1) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "rx_channel = %d, ring_size = %d",
			    rx->channel_number, rx->ring_size);
		ret = -EINVAL;
		goto cleanup;
	}

	if ((rx->ring_size) & (~RX_CFG_B_RX_RING_LEN_MASK_)) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "ring size is too large, rx_channel = %d",
			    rx->channel_number);
		ret = -EINVAL;
		goto cleanup;
	}

	ring_allocation_size = ALIGN(rx->ring_size * descriptor_spacing,
				     PAGE_SIZE);

	dma_ptr = 0;
	cpu_ptr = pci_zalloc_consistent(rx->adapter->pci.pdev,
					ring_allocation_size, &dma_ptr);
	if (!cpu_ptr) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Failed to allocate rx ring, channel = %d",
			    rx->channel_number);
		ret = -ENOMEM;
		goto cleanup;
	}
	NETIF_ASSERT(adapter, drv, adapter->netdev, dma_ptr);
	rx->ring_allocation_size = ring_allocation_size;
	rx->ring_cpu_ptr = (struct lan743x_rx_descriptor *)cpu_ptr;
	rx->ring_dma_ptr = dma_ptr;
	if ((rx->ring_dma_ptr) & 0x3) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "dma ring base is not DWORD aligned, channel = %d",
			    rx->channel_number);
		ret = -ENOMEM;
		goto cleanup;
	}

	cpu_ptr = kzalloc(rx->ring_size * sizeof(*rx->buffer_info),
			  GFP_KERNEL);
	if (!cpu_ptr) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Failed to allocate buffer info, channel = %d",
			    rx->channel_number);
		ret = -ENOMEM;
		goto cleanup;
	}
	rx->buffer_info = (struct lan743x_rx_buffer_info *)cpu_ptr;

	dma_ptr = 0;
	cpu_ptr = pci_zalloc_consistent(
		rx->adapter->pci.pdev,
		sizeof(*rx->head_cpu_ptr), &dma_ptr);
	if (!cpu_ptr) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Failed to allocate head pointer, channel = %d",
			    rx->channel_number);
		ret = -ENOMEM;
		goto cleanup;
	}
	NETIF_ASSERT(adapter, drv, adapter->netdev, dma_ptr);
	rx->head_cpu_ptr = cpu_ptr;
	rx->head_dma_ptr = dma_ptr;
	if ((rx->head_dma_ptr) & 0x3) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "head write back pointer is not DWORD aligned, channel = %d",
			    rx->channel_number);
		ret = -ENOMEM;
		goto cleanup;
	}
	rx->last_head = 0;

	for (element_index = 0; element_index < rx->ring_size;
	     element_index++) {
		ret = lan743x_rx_allocate_ring_element(rx, element_index);
		if (ret) {
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "failed to allocate rx ring element, element_index = %d",
				    element_index);
			goto cleanup;
		}
	}
	ret = 0;
cleanup:
	if (ret)
		lan743x_rx_ring_cleanup(rx);

	return ret;
}

static void lan743x_rx_ring_cleanup(struct lan743x_rx *rx)
{
	struct lan743x_adapter *adapter = rx->adapter;

	NETIF_ASSERT(adapter, drv, adapter->netdev, rx->adapter);

	if ((rx->buffer_info) && (rx->ring_cpu_ptr)) {
		int element_index;

		for (element_index = 0; element_index < rx->ring_size;
		     element_index++)
			lan743x_rx_release_ring_element(rx, element_index);
	}

	if (rx->head_cpu_ptr) {
		pci_free_consistent(rx->adapter->pci.pdev,
				    sizeof(*rx->head_cpu_ptr),
				    (void *)(rx->head_cpu_ptr),
				    rx->head_dma_ptr);
		rx->head_cpu_ptr = NULL;
		rx->head_dma_ptr = 0;
	}

	kfree(rx->buffer_info);
	rx->buffer_info = NULL;

	if (rx->ring_cpu_ptr) {
		pci_free_consistent(rx->adapter->pci.pdev,
				    rx->ring_allocation_size,
				    rx->ring_cpu_ptr,
				    rx->ring_dma_ptr);
		rx->ring_allocation_size = 0;
		rx->ring_cpu_ptr = NULL;
		rx->ring_dma_ptr = 0;
	}

	rx->ring_size = 0;
	rx->last_head = 0;
}

static int lan743x_rx_init(struct lan743x_rx *rx,
			   struct lan743x_adapter *adapter, int rx_channel)
{
	int ret = -ENODEV;

	memset(rx, 0, sizeof(*rx));
	NETIF_ASSERT(adapter, probe, adapter->netdev,
		     (rx_channel >= 0) &&
		     (rx_channel < LAN743X_NUMBER_OF_RX_CHANNELS));

	rx->adapter = adapter;
	rx->channel_number = -1;

	ret = lan743x_dmac_reserve_rx_channel(adapter, rx_channel);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "Failed to reserve rx channel %d",
			    rx_channel);
		goto cleanup;
	}
	rx->channel_number = rx_channel;

	ret = 0;

cleanup:
	if (ret)
		lan743x_rx_cleanup(rx);
	return ret;
}

static void lan743x_rx_cleanup(struct lan743x_rx *rx)
{
	struct lan743x_adapter *adapter = rx->adapter;

	if (rx->channel_number >= 0) {
		lan743x_dmac_release_rx_channel(adapter, rx->channel_number);
		rx->channel_number = -1;
	}

	memset(rx, 0, sizeof(*rx));
}

static int lan743x_rx_open(struct lan743x_rx *rx)
{
	int ret = -ENODEV;
	struct lan743x_adapter *adapter = rx->adapter;
	u32 data = 0;

	NETIF_ASSERT(adapter, ifup, adapter->netdev,
		     (rx->channel_number >= 0) &&
		     (rx->channel_number < LAN743X_NUMBER_OF_RX_CHANNELS));

	ret = lan743x_rx_ring_init(rx);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "Rx channel %d, ring initialization failed",
			    rx->channel_number);
		goto cleanup;
	}
	rx->flags |= RX_FLAG_RING_ALLOCATED;

	NETIF_ASSERT(adapter, ifup, adapter->netdev, rx->ring_size >= 1);

	netif_napi_add(rx->adapter->netdev,
		       &rx->napi, lan743x_rx_napi_poll,
		       rx->ring_size - 1);
	rx->flags |= RX_FLAG_NAPI_ADDED;

	ret = lan743x_dmac_rx_reset(adapter, rx->channel_number);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "Failed to reset rx dmac, rx_channel = %d",
			    rx->channel_number);
		goto cleanup;
	}

	/* set ring base address */
	NETIF_ASSERT(adapter, ifup, adapter->netdev,
		     !((rx->ring_dma_ptr) & 0x3));
	lan743x_csr_write(adapter,
			  RX_BASE_ADDRH(rx->channel_number),
			  DMA_ADDR_HIGH32(rx->ring_dma_ptr));
	lan743x_csr_write(adapter,
			  RX_BASE_ADDRL(rx->channel_number),
			  DMA_ADDR_LOW32(rx->ring_dma_ptr));

	/* set rx write back address */
	NETIF_ASSERT(adapter, ifup, adapter->netdev,
		     !((rx->head_dma_ptr) & 0x3));
	lan743x_csr_write(adapter,
			  RX_HEAD_WRITEBACK_ADDRH(rx->channel_number),
			  DMA_ADDR_HIGH32(rx->head_dma_ptr));
	lan743x_csr_write(adapter,
			  RX_HEAD_WRITEBACK_ADDRL(rx->channel_number),
			  DMA_ADDR_LOW32(rx->head_dma_ptr));

	/* set RX_CFG_A */
	lan743x_csr_write(adapter,
			  RX_CFG_A(rx->channel_number),
			  RX_CFG_A_RX_HP_WB_EN_);

	/* set RX_CFG_B */
	data = lan743x_csr_read(adapter, RX_CFG_B(rx->channel_number));
	data &= ~RX_CFG_B_RX_PAD_MASK_;
	if (!RX_HEAD_PADDING)
		data |= RX_CFG_B_RX_PAD_0_;
	else
		data |= RX_CFG_B_RX_PAD_2_;
	data &= ~RX_CFG_B_RX_RING_LEN_MASK_;
	data |= ((rx->ring_size) & RX_CFG_B_RX_RING_LEN_MASK_);
	data |= RX_CFG_B_TS_ALL_RX_;
	lan743x_csr_write(adapter, RX_CFG_B(rx->channel_number), data);

	lan743x_csr_write(adapter, RX_TAIL(rx->channel_number),
			  ((u32)(rx->ring_size - 1)));
	rx->last_head = lan743x_csr_read(adapter, RX_HEAD(rx->channel_number));
	if (rx->last_head) {
		NETIF_ERROR(adapter, ifup, adapter->netdev, "last_head != 0");
		ret = -EIO;
		goto cleanup;
	}

	napi_enable(&rx->napi);
	lan743x_csr_write(adapter, INT_EN_SET,
			  INT_BIT_DMA_RX_(rx->channel_number));
	lan743x_csr_write(adapter, DMAC_INT_STS,
			  DMAC_INT_BIT_RXFRM_(rx->channel_number));
	lan743x_csr_write(adapter, DMAC_INT_EN_SET,
			  DMAC_INT_BIT_RXFRM_(rx->channel_number));
	rx->flags |= RX_FLAG_ISR_ENABLED;

	ret = lan743x_dmac_rx_start(adapter, rx->channel_number);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "Failed to start rx channel %d, first",
			    rx->channel_number);
		goto cleanup;
	}
	rx->flags |= RX_FLAG_DMAC_STARTED;

	/* initialize fifo */
	NETIF_ASSERT(adapter, ifup, adapter->netdev,
		     !(rx->flags & RX_FLAG_FIFO_ENABLED));
	ret = lan743x_fct_rx_reset(adapter, rx->channel_number);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "Failed to reset rx fifo, rx_channel = %d",
			    rx->channel_number);
		goto cleanup;
	}

	/* enable fifo */
	ret = lan743x_fct_rx_enable(adapter, rx->channel_number);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "Failed to enable rx fifo, rx_channel = %d",
			    rx->channel_number);
		goto cleanup;
	}
	rx->flags |= RX_FLAG_FIFO_ENABLED;

	/* enable mac */
	ret = lan743x_mac_rx_enable(adapter, rx->channel_number);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "failed to enable mac, rx_channel = %d",
			    rx->channel_number);
		goto cleanup;
	}
	rx->flags |= RX_FLAG_MAC_ENABLED;

	ret = 0;
cleanup:
	if (ret)
		lan743x_rx_close(rx);
	return ret;
}

static void lan743x_rx_close(struct lan743x_rx *rx)
{
	struct lan743x_adapter *adapter = rx->adapter;

	if (rx->flags & RX_FLAG_MAC_ENABLED) {
		lan743x_mac_rx_disable(adapter, rx->channel_number);
		rx->flags &= ~RX_FLAG_MAC_ENABLED;
	}

	if (rx->flags & RX_FLAG_FIFO_ENABLED) {
		lan743x_fct_rx_disable(adapter, rx->channel_number);
		rx->flags &= ~RX_FLAG_FIFO_ENABLED;
	}

	if (rx->flags & RX_FLAG_DMAC_STARTED) {
		lan743x_dmac_rx_stop(adapter, rx->channel_number);
		rx->flags &= ~RX_FLAG_DMAC_STARTED;
	}

	if (rx->flags & RX_FLAG_ISR_ENABLED) {
		lan743x_csr_write(adapter,
				  DMAC_INT_EN_CLR,
				  DMAC_INT_BIT_RXFRM_(rx->channel_number));
		lan743x_csr_write(adapter, INT_EN_CLR,
				  INT_BIT_DMA_RX_(rx->channel_number));
		napi_disable(&rx->napi);
		rx->flags &= ~RX_FLAG_ISR_ENABLED;
	}

	if (rx->flags & RX_FLAG_NAPI_ADDED) {
		netif_napi_del(&rx->napi);
		rx->flags &= ~RX_FLAG_NAPI_ADDED;
	}

	if (rx->flags & RX_FLAG_RING_ALLOCATED) {
		lan743x_rx_ring_cleanup(rx);
		rx->flags &= ~RX_FLAG_RING_ALLOCATED;
	}
}

/* NETDEV */

static int lan743x_netdev_close(struct net_device *netdev)
{
	struct lan743x_adapter *adapter = netdev_priv(netdev);

	NETIF_INFO(adapter, ifdown, adapter->netdev, "LAN743x_closing");

	if (adapter->open_flags & LAN743X_COMPONENT_FLAG_TX(0)) {
		lan743x_tx_close(&adapter->tx[0]);
		adapter->open_flags &= ~LAN743X_COMPONENT_FLAG_TX(0);
	}
	if (adapter->open_flags & LAN743X_COMPONENT_FLAG_RX(0)) {
		lan743x_rx_close(&adapter->rx[0]);
		adapter->open_flags &= ~LAN743X_COMPONENT_FLAG_RX(0);
	}
	if (adapter->open_flags & LAN743X_COMPONENT_FLAG_DMAC) {
		lan743x_dmac_close(adapter);
		adapter->open_flags &= ~LAN743X_COMPONENT_FLAG_DMAC;
	}
	if (adapter->open_flags & LAN743X_COMPONENT_FLAG_FCT) {
		lan743x_fct_close(adapter);
		adapter->open_flags &= ~LAN743X_COMPONENT_FLAG_FCT;
	}
	if (adapter->open_flags & LAN743X_COMPONENT_FLAG_RFE) {
		lan743x_rfe_close(adapter);
		adapter->open_flags &= ~LAN743X_COMPONENT_FLAG_RFE;
	}
	if (adapter->open_flags & LAN743X_COMPONENT_FLAG_PTP) {
		lan743x_ptp_close(adapter);
		adapter->open_flags &= ~LAN743X_COMPONENT_FLAG_PTP;
	}
	if (adapter->open_flags & LAN743X_COMPONENT_FLAG_PHY) {
		lan743x_phy_close(adapter);
		adapter->open_flags &= ~LAN743X_COMPONENT_FLAG_PHY;
	}
	if (adapter->open_flags & LAN743X_COMPONENT_FLAG_MAC) {
		lan743x_mac_close(adapter);
		adapter->open_flags &= ~LAN743X_COMPONENT_FLAG_MAC;
	}
	if (adapter->open_flags & LAN743X_COMPONENT_FLAG_GPIO) {
		lan743x_gpio_close(adapter);
		adapter->open_flags &= ~LAN743X_COMPONENT_FLAG_GPIO;
	}
	if (adapter->open_flags & LAN743X_COMPONENT_FLAG_DP) {
		lan743x_dp_close(adapter);
		adapter->open_flags &= ~LAN743X_COMPONENT_FLAG_DP;
	}
	if (adapter->open_flags & LAN743X_COMPONENT_FLAG_INTR) {
		lan743x_intr_close(adapter);
		adapter->open_flags &= ~LAN743X_COMPONENT_FLAG_INTR;
	}
	return 0;
}

static int lan743x_netdev_open(struct net_device *netdev)
{
	struct lan743x_adapter *adapter = netdev_priv(netdev);
	int ret;

	NETIF_ASSERT(adapter, ifup, adapter->netdev, !adapter->open_flags);

	ret = lan743x_intr_open(adapter);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "intr opened failed");
		goto clean_up;
	}
	adapter->open_flags |= LAN743X_COMPONENT_FLAG_INTR;

	ret = lan743x_dp_open(adapter);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev, "dp_open failed");
		goto clean_up;
	}
	adapter->open_flags |= LAN743X_COMPONENT_FLAG_DP;

	ret = lan743x_gpio_open(adapter);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "gpio_open failed");
		goto clean_up;
	}
	adapter->open_flags |= LAN743X_COMPONENT_FLAG_GPIO;

	ret = lan743x_mac_open(adapter);
	if (ret) {
		NETIF_ERROR(adapter, drv, adapter->netdev, "mac_open failed");
		goto clean_up;
	}
	adapter->open_flags |= LAN743X_COMPONENT_FLAG_MAC;

	ret = lan743x_phy_open(adapter);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev, "phy_open failed");
		goto clean_up;
	}
	adapter->open_flags |= LAN743X_COMPONENT_FLAG_PHY;

	ret = lan743x_ptp_open(adapter);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev, "ptp_open failed");
		goto clean_up;
	}
	adapter->open_flags |= LAN743X_COMPONENT_FLAG_PTP;

	ret = lan743x_rfe_open(adapter);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev, "rfe_open failed");
		goto clean_up;
	}
	adapter->open_flags |= LAN743X_COMPONENT_FLAG_RFE;

	ret = lan743x_fct_open(adapter);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev, "fct_open failed");
		goto clean_up;
	}
	adapter->open_flags |= LAN743X_COMPONENT_FLAG_FCT;

	ret = lan743x_dmac_open(adapter);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "dmac_open failed");
		goto clean_up;
	}
	adapter->open_flags |= LAN743X_COMPONENT_FLAG_DMAC;

	ret = lan743x_rx_open(&adapter->rx[0]);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "rx[0] open failed");
		goto clean_up;
	}
	adapter->open_flags |= LAN743X_COMPONENT_FLAG_RX(0);

	ret = lan743x_tx_open(&adapter->tx[0]);
	if (ret) {
		NETIF_ERROR(adapter, ifup, adapter->netdev,
			    "tx[0] open failed");
		goto clean_up;
	}
	adapter->open_flags |= LAN743X_COMPONENT_FLAG_TX(0);

	NETIF_INFO(adapter, ifup, adapter->netdev,
		   "LAN743x opened successfully");

clean_up:
	if (ret) {
		NETIF_WARNING(adapter, ifup, adapter->netdev,
			      "Error opening LAN743x, performing cleanup");
		lan743x_netdev_close(netdev);
	}
	return ret;
}

static netdev_tx_t lan743x_netdev_xmit_frame(
	struct sk_buff *skb, struct net_device *netdev)
{
	struct lan743x_adapter *adapter = netdev_priv(netdev);

	netdev->stats.tx_packets++;
	return lan743x_tx_xmit_frame(&adapter->tx[0], skb);
}

static int lan743x_netdev_ioctl(
	struct net_device *netdev, struct ifreq *ifr, int cmd)
{
	struct lan743x_adapter *adapter = netdev_priv(netdev);
	struct hwtstamp_config config;
	int ret = 0;
	int index;

	if (!netif_running(netdev))
		return -EINVAL;
	if (cmd != SIOCSHWTSTAMP) {
		int ret = phy_mii_ioctl(netdev->phydev, ifr, cmd);

		if (ret == -EINVAL)
			NETIF_ERROR(adapter, drv, adapter->netdev,
				    "operation not supported");
		return ret;
	}
	if (!ifr) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "SIOCSHWTSTAMP, ifr == NULL");
		return -EINVAL;
	}

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	if (config.flags) {
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "ignoring hwtstamp_config.flags == 0x%08X, expected 0",
			      config.flags);
	}

	switch (config.tx_type) {
	case HWTSTAMP_TX_OFF:
		for (index = 0; index < LAN743X_NUMBER_OF_TX_CHANNELS;
		     index++)
			lan743x_tx_set_timestamping_enable(&adapter->tx[index],
							   false);
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  tx_type = HWTSTAMP_TX_OFF");
		break;
	case HWTSTAMP_TX_ON:
		for (index = 0; index < LAN743X_NUMBER_OF_TX_CHANNELS;
		     index++)
			lan743x_tx_set_timestamping_enable(&adapter->tx[index],
							   true);
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  tx_type = HWTSTAMP_TX_ON");
		break;
	default:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  tx_type = %d, UNKNOWN", config.tx_type);
		ret = -EINVAL;
		break;
	}
	/* currently the driver timestamps all incoming packets
	 * so no special setting is require
	 */
	switch (config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_NONE");
		break;
	case HWTSTAMP_FILTER_ALL:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_ALL");
		break;
	case HWTSTAMP_FILTER_SOME:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_SOME");
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_PTP_V1_L4_EVENT");
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_PTP_V1_L4_SYNC");
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ");
		break;
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_EVENT");
		break;
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_SYNC");
		break;
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ");
		break;
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT");
		break;
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_SYNC");
		break;
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ");
		break;
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT");
		break;
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_PTP_V2_SYNC");
		break;
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		NETIF_INFO(adapter, drv, adapter->netdev,
			   "  rx_filter = HWTSTAMP_FILTER_PTP_V2_DELAY_REQ");
		break;
	default:
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "  rx_filter = %d, UNKNOWN", config.rx_filter);
		NETIF_WARNING(adapter, drv, adapter->netdev,
			      "  assuming rx_filter = HWTSTAMP_FILTER_ALL");
		/* treat this like HWTSTAMP_FILTER_ALL */
		break;
	}
	if (!ret)
		return copy_to_user(ifr->ifr_data, &config,
				    sizeof(config)) ? -EFAULT : 0;
	return ret;
}

static void lan743x_netdev_set_multicast(struct net_device *netdev)
{
	struct lan743x_adapter *adapter = netdev_priv(netdev);

	NETIF_ASSERT(adapter, drv, adapter->netdev, netdev);
	adapter = netdev_priv(netdev);
	lan743x_rfe_set_multicast(adapter);
}

static int lan743x_netdev_change_mtu(struct net_device *netdev, int new_mtu)
{
	int ret = 0;
	struct lan743x_adapter *adapter = netdev_priv(netdev);

	NETIF_INFO(adapter, drv, adapter->netdev, "new_mtu = %d", new_mtu);
	ret = lan743x_mac_set_mtu(adapter, new_mtu);
	if (!ret)
		netdev->mtu = new_mtu;
	return ret;
}

static struct net_device_stats *lan743x_netdev_get_stats(struct net_device *nd)
{
	struct lan743x_adapter *adapter = NULL;

	NETIF_ASSERT(adapter, drv, adapter->netdev, nd);
	adapter = netdev_priv(nd);
	return mac_get_stats(adapter);
}

static int lan743x_netdev_set_mac_address(struct net_device *netdev,
					  void *addr)
{
	struct lan743x_adapter *adapter = NULL;
	struct sockaddr *sock_addr = addr;

	NETIF_ASSERT(adapter, drv, adapter->netdev, netdev);

	if (netif_running(netdev))
		return -EBUSY;

	if (!is_valid_ether_addr(sock_addr->sa_data))
		return -EADDRNOTAVAIL;

	ether_addr_copy(netdev->dev_addr, sock_addr->sa_data);

	adapter = netdev_priv(netdev);
	lan743x_mac_set_address(adapter, sock_addr->sa_data);
	lan743x_rfe_update_mac_address(adapter);

	return 0;
}

static const struct net_device_ops lan743x_netdev_ops = {
	.ndo_open		= lan743x_netdev_open,
	.ndo_stop		= lan743x_netdev_close,
	.ndo_start_xmit		= lan743x_netdev_xmit_frame,
	.ndo_do_ioctl		= lan743x_netdev_ioctl,
	.ndo_set_rx_mode	= lan743x_netdev_set_multicast,
	.ndo_change_mtu		= lan743x_netdev_change_mtu,
	.ndo_get_stats		= lan743x_netdev_get_stats,
	.ndo_set_mac_address	= lan743x_netdev_set_mac_address,
};

/* ETHTOOL */
static const char lan743x_gstrings[][ETH_GSTRING_LEN] = {
	"RX FCS Errors",
	"RX Alignment Errors",
	"Rx Fragment Errors",
	"RX Jabber Errors",
	"RX Undersize Frame Errors",
	"RX Oversize Frame Errors",
	"RX Dropped Frames",
	"RX Unicast Byte Count",
	"RX Broadcast Byte Count",
	"RX Multicast Byte Count",
	"RX Unicast Frames",
	"RX Broadcast Frames",
	"RX Multicast Frames",
	"RX Pause Frames",
	"RX 64 Byte Frames",
	"RX 65 - 127 Byte Frames",
	"RX 128 - 255 Byte Frames",
	"RX 256 - 511 Bytes Frames",
	"RX 512 - 1023 Byte Frames",
	"RX 1024 - 1518 Byte Frames",
	"RX Greater 1518 Byte Frames",
	"RX Total Frames",
	"EEE RX LPI Transitions",
	"EEE RX LPI Time",
	"RX Counter Rollover Status",
	"TX FCS Errors",
	"TX Excess Deferral Errors",
	"TX Carrier Errors",
	"TX Bad Byte Count",
	"TX Single Collisions",
	"TX Multiple Collisions",
	"TX Excessive Collision",
	"TX Late Collisions",
	"TX Unicast Byte Count",
	"TX Broadcast Byte Count",
	"TX Multicast Byte Count",
	"TX Unicast Frames",
	"TX Broadcast Frames",
	"TX Multicast Frames",
	"TX Pause Frames",
	"TX 64 Byte Frames",
	"TX 65 - 127 Byte Frames",
	"TX 128 - 255 Byte Frames",
	"TX 256 - 511 Bytes Frames",
	"TX 512 - 1023 Byte Frames",
	"TX 1024 - 1518 Byte Frames",
	"TX Greater 1518 Byte Frames",
	"TX Total Frames",
	"EEE TX LPI Transitions",
	"EEE TX LPI Time",
	"TX Counter Rollover Status",
};

static const u32 lan743x_stat_addr[] = {
	STAT_RX_FCS_ERRORS,
	STAT_RX_ALIGNMENT_ERRORS,
	STAT_RX_FRAGMENT_ERRORS,
	STAT_RX_JABBER_ERRORS,
	STAT_RX_UNDERSIZE_FRAME_ERRORS,
	STAT_RX_OVERSIZE_FRAME_ERRORS,
	STAT_RX_DROPPED_FRAMES,
	STAT_RX_UNICAST_BYTE_COUNT,
	STAT_RX_BROADCAST_BYTE_COUNT,
	STAT_RX_MULTICAST_BYTE_COUNT,
	STAT_RX_UNICAST_FRAMES,
	STAT_RX_BROADCAST_FRAMES,
	STAT_RX_MULTICAST_FRAMES,
	STAT_RX_PAUSE_FRAMES,
	STAT_RX_64_BYTE_FRAMES,
	STAT_RX_65_127_BYTE_FRAMES,
	STAT_RX_128_255_BYTE_FRAMES,
	STAT_RX_256_511_BYTES_FRAMES,
	STAT_RX_512_1023_BYTE_FRAMES,
	STAT_RX_1024_1518_BYTE_FRAMES,
	STAT_RX_GREATER_1518_BYTE_FRAMES,
	STAT_RX_TOTAL_FRAMES,
	STAT_EEE_RX_LPI_TRANSITIONS,
	STAT_EEE_RX_LPI_TIME,
	STAT_RX_COUNTER_ROLLOVER_STATUS,
	STAT_TX_FCS_ERRORS,
	STAT_TX_EXCESS_DEFERRAL_ERRORS,
	STAT_TX_CARRIER_ERRORS,
	STAT_TX_BAD_BYTE_COUNT,
	STAT_TX_SINGLE_COLLISIONS,
	STAT_TX_MULTIPLE_COLLISIONS,
	STAT_TX_EXCESSIVE_COLLISION,
	STAT_TX_LATE_COLLISIONS,
	STAT_TX_UNICAST_BYTE_COUNT,
	STAT_TX_BROADCAST_BYTE_COUNT,
	STAT_TX_MULTICAST_BYTE_COUNT,
	STAT_TX_UNICAST_FRAMES,
	STAT_TX_BROADCAST_FRAMES,
	STAT_TX_MULTICAST_FRAMES,
	STAT_TX_PAUSE_FRAMES,
	STAT_TX_64_BYTE_FRAMES,
	STAT_TX_65_127_BYTE_FRAMES,
	STAT_TX_128_255_BYTE_FRAMES,
	STAT_TX_256_511_BYTES_FRAMES,
	STAT_TX_512_1023_BYTE_FRAMES,
	STAT_TX_1024_1518_BYTE_FRAMES,
	STAT_TX_GREATER_1518_BYTE_FRAMES,
	STAT_TX_TOTAL_FRAMES,
	STAT_EEE_TX_LPI_TRANSITIONS,
	STAT_EEE_TX_LPI_TIME,
	STAT_TX_COUNTER_ROLLOVER_STATUS
};

static void lan743x_ethtool_get_drvinfo(struct net_device *netdev,
					struct ethtool_drvinfo *info)
{
	struct lan743x_adapter *adapter = netdev_priv(netdev);

	strlcpy(info->driver, DRIVER_NAME, sizeof(info->driver));
	strlcpy(info->version, DRIVER_VERSION, sizeof(info->version));
	strlcpy(info->bus_info,
		pci_name(adapter->pci.pdev), sizeof(info->bus_info));
}

static u32 lan743x_ethtool_get_msglevel(struct net_device *netdev)
{
	struct lan743x_adapter *adapter = netdev_priv(netdev);

	NETIF_INFO(adapter, drv, adapter->netdev,
		   "get_msglevel: msg_enable == 0x%08X",
		   adapter->msg_enable);
	return adapter->msg_enable;
}

static void lan743x_ethtool_set_msglevel(struct net_device *netdev,
					 u32 msglevel)
{
	struct lan743x_adapter *adapter = netdev_priv(netdev);

	adapter->msg_enable = msglevel;
	NETIF_INFO(adapter, drv, adapter->netdev,
		   "set_msglevel: msg_enable == 0x%08X",
		   adapter->msg_enable);
}

static int lan743x_ethtool_get_eeprom_len(struct net_device *netdev)
{
	return 0;
}

static void lan743x_ethtool_get_strings(struct net_device *netdev,
					u32 stringset, u8 *data)
{
	switch (stringset) {
	case ETH_SS_STATS:
		memcpy(data, lan743x_gstrings, sizeof(lan743x_gstrings));
		break;
	}
}

static void lan743x_ethtool_get_ethtool_stats(struct net_device *netdev,
					      struct ethtool_stats *stats,
					      u64 *data)
{
	struct lan743x_adapter *adapter = netdev_priv(netdev);
	int i;
	u32 buf;

	for (i = 0; i < (sizeof(lan743x_stat_addr) / (sizeof(u32))); i++) {
		buf = lan743x_csr_read(adapter, lan743x_stat_addr[i]);
		data[i] = (u64)buf;
	}
}

static int lan743x_ethtool_get_sset_count(struct net_device *netdev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		return ARRAY_SIZE(lan743x_gstrings);
	default:
		return -EOPNOTSUPP;
	}
}

static int lan743x_ethtool_get_ts_info(struct net_device *netdev,
				       struct ethtool_ts_info *ts_info)
{
	struct lan743x_adapter *adapter = netdev_priv(netdev);

	ts_info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE |
				   SOF_TIMESTAMPING_RX_SOFTWARE |
				   SOF_TIMESTAMPING_SOFTWARE |
				   SOF_TIMESTAMPING_TX_HARDWARE |
				   SOF_TIMESTAMPING_RX_HARDWARE |
				   SOF_TIMESTAMPING_RAW_HARDWARE;
#ifdef CONFIG_PTP_1588_CLOCK
	ts_info->phc_index = lan743x_ptp_get_clock_index(adapter);
#else
	ts_info->phc_index = -1;
#endif
	ts_info->tx_types = BIT(HWTSTAMP_TX_OFF) |
			    BIT(HWTSTAMP_TX_ON);
	ts_info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) |
			      BIT(HWTSTAMP_FILTER_ALL);
	return 0;
}

static int lan743x_ethtool_get_eee(struct net_device *netdev,
				   struct ethtool_eee *eee)
{
	struct lan743x_adapter *adapter = netdev_priv(netdev);
	struct phy_device *phydev = netdev->phydev;
	u32 buf;
	int ret;

	if (!phydev)
		return -EIO;
	if (!phydev->drv) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Missing PHY Driver");
		return -EIO;
	}

	ret = phy_ethtool_get_eee(phydev, eee);
	if (ret < 0)
		return ret;

	buf = lan743x_csr_read(adapter, MAC_CR);
	if (buf & MAC_CR_EEE_EN_) {
		eee->eee_enabled = true;
		eee->eee_active = !!(eee->advertised & eee->lp_advertised);
		eee->tx_lpi_enabled = true;
		/* EEE_TX_LPI_REQ_DLY & tx_lpi_timer are same uSec unit */
		buf = lan743x_csr_read(adapter, MAC_EEE_TX_LPI_REQ_DLY_CNT);
		eee->tx_lpi_timer = buf;
	} else {
		eee->eee_enabled = false;
		eee->eee_active = false;
		eee->tx_lpi_enabled = false;
		eee->tx_lpi_timer = 0;
	}

	return 0;
}

static int lan743x_ethtool_set_eee(struct net_device *netdev,
				   struct ethtool_eee *eee)
{
	struct lan743x_adapter *adapter = netdev_priv(netdev);
	struct phy_device *phydev = NULL;
	u32 buf = 0;

	if (!netdev)
		return -EINVAL;
	adapter = netdev_priv(netdev);
	if (!adapter)
		return -EINVAL;
	phydev = netdev->phydev;
	if (!phydev)
		return -EIO;
	if (!phydev->drv) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "Missing PHY Driver");
		return -EIO;
	}

	if (eee->eee_enabled) {
		buf = lan743x_csr_read(adapter, MAC_CR);
		buf |= MAC_CR_EEE_EN_;
		lan743x_csr_write(adapter, MAC_CR, buf);

		phy_ethtool_set_eee(phydev, eee);

		buf = (u32)eee->tx_lpi_timer;
		lan743x_csr_write(adapter, MAC_EEE_TX_LPI_REQ_DLY_CNT, buf);
		NETIF_INFO(adapter, drv, adapter->netdev, "Enabled EEE");
	} else {
		buf = lan743x_csr_read(adapter, MAC_CR);
		buf &= ~MAC_CR_EEE_EN_;
		lan743x_csr_write(adapter, MAC_CR, buf);
		NETIF_INFO(adapter, drv, adapter->netdev, "Disabled EEE");
	}

	return 0;
}

static const struct ethtool_ops lan743x_ethtool_ops = {
	.get_drvinfo            = lan743x_ethtool_get_drvinfo,
	.get_msglevel           = lan743x_ethtool_get_msglevel,
	.set_msglevel           = lan743x_ethtool_set_msglevel,
	.get_link               = ethtool_op_get_link,

	.get_eeprom_len         = lan743x_ethtool_get_eeprom_len,
	.get_strings            = lan743x_ethtool_get_strings,
	.get_ethtool_stats      = lan743x_ethtool_get_ethtool_stats,
	.get_sset_count         = lan743x_ethtool_get_sset_count,
	.get_ts_info            = lan743x_ethtool_get_ts_info,
	.get_eee                = lan743x_ethtool_get_eee,
	.set_eee                = lan743x_ethtool_set_eee,
	.get_link_ksettings     = phy_ethtool_get_link_ksettings,
	.set_link_ksettings     = phy_ethtool_set_link_ksettings
};

static void lan743x_device_cleanup(struct lan743x_adapter *adapter)
{
	struct net_device *netdev = NULL;

	NETIF_INFO(adapter, drv, adapter->netdev, "performing cleanup");

	if (adapter->init_flags & LAN743X_INIT_FLAG_NETDEV_REGISTERED) {
		unregister_netdev(adapter->netdev);
		adapter->init_flags &= ~LAN743X_INIT_FLAG_NETDEV_REGISTERED;
	}
	if (adapter->init_flags & LAN743X_COMPONENT_FLAG_TX(0)) {
		lan743x_tx_cleanup(&adapter->tx[0]);
		adapter->init_flags &= ~LAN743X_COMPONENT_FLAG_TX(0);
	}
	if (adapter->init_flags & LAN743X_COMPONENT_FLAG_RX(0)) {
		lan743x_rx_cleanup(&adapter->rx[0]);
		adapter->init_flags &= ~LAN743X_COMPONENT_FLAG_RX(0);
	}
	if (adapter->init_flags & LAN743X_COMPONENT_FLAG_DMAC) {
		lan743x_dmac_cleanup(adapter);
		adapter->init_flags &= ~LAN743X_COMPONENT_FLAG_DMAC;
	}
	if (adapter->init_flags & LAN743X_COMPONENT_FLAG_FCT) {
		lan743x_fct_cleanup(adapter);
		adapter->init_flags &= ~LAN743X_COMPONENT_FLAG_FCT;
	}
	if (adapter->init_flags & LAN743X_COMPONENT_FLAG_RFE) {
		lan743x_rfe_cleanup(adapter);
		adapter->init_flags &= ~LAN743X_COMPONENT_FLAG_RFE;
	}
	if (adapter->init_flags & LAN743X_COMPONENT_FLAG_PTP) {
		lan743x_ptp_cleanup(adapter);
		adapter->init_flags &= ~LAN743X_COMPONENT_FLAG_PTP;
	}
	if (adapter->init_flags & LAN743X_COMPONENT_FLAG_PHY) {
		lan743x_phy_cleanup(adapter);
		adapter->init_flags &= ~LAN743X_COMPONENT_FLAG_PHY;
	}
	if (adapter->init_flags & LAN743X_COMPONENT_FLAG_MAC) {
		lan743x_mac_cleanup(adapter);
		adapter->init_flags &= ~LAN743X_COMPONENT_FLAG_MAC;
	}
	if (adapter->init_flags & LAN743X_COMPONENT_FLAG_GPIO) {
		lan743x_gpio_cleanup(adapter);
		adapter->init_flags &= ~LAN743X_COMPONENT_FLAG_GPIO;
	}
	if (adapter->init_flags & LAN743X_COMPONENT_FLAG_DP) {
		lan743x_dp_cleanup(adapter);
		adapter->init_flags &= ~LAN743X_COMPONENT_FLAG_DP;
	}
	if (adapter->init_flags & LAN743X_COMPONENT_FLAG_INTR) {
		lan743x_intr_cleanup(adapter);
		adapter->init_flags &= ~LAN743X_COMPONENT_FLAG_INTR;
	}
	if (adapter->init_flags & LAN743X_COMPONENT_FLAG_CSR) {
		lan743x_csr_cleanup(adapter);
		adapter->init_flags &= ~LAN743X_COMPONENT_FLAG_CSR;
	}
	if (adapter->init_flags & LAN743X_COMPONENT_FLAG_PCI) {
		lan743x_pci_cleanup(adapter);
		adapter->init_flags &= ~LAN743X_COMPONENT_FLAG_PCI;
	}

	netdev = adapter->netdev;
	memset(adapter, 0, sizeof(struct lan743x_adapter));
	free_netdev(netdev);
}

/* lan743x_pcidev_probe - Device Initialization Routine
 * @pdev: PCI device information struct
 * @id: entry in lan743x_pci_tbl
 *
 * Returns 0 on success, negative on failure
 *
 * initializes an adapter identified by a pci_dev structure.
 * The OS initialization, configuring of the adapter private structure,
 * and a hardware reset occur.
 **/
static int lan743x_pcidev_probe(struct pci_dev *pdev,
				const struct pci_device_id *id)
{
	struct net_device *netdev = NULL;
	struct lan743x_adapter *adapter = NULL;
	int ret = -ENODEV;

	NETIF_ASSERT(adapter, probe, adapter->netdev, pdev);

	netdev = alloc_etherdev(sizeof(struct lan743x_adapter));
	if (!netdev) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "alloc_etherdev returned NULL");
		ret = -ENOMEM;
		goto clean_up;
	}

	strncpy(netdev->name, pci_name(pdev), sizeof(netdev->name) - 1);
	SET_NETDEV_DEV(netdev, &pdev->dev);
	pci_set_drvdata(pdev, netdev);
	adapter = netdev_priv(netdev);
	if (!adapter) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "netdev_priv returned NULL");
		ret = -ENOMEM;
		goto clean_up;
	}
	memset(adapter, 0, sizeof(struct lan743x_adapter));
	adapter->netdev = netdev;
	adapter->init_flags = 0;
	adapter->open_flags = 0;
	adapter->msg_enable = msg_enable;
	netdev->max_mtu = LAN743X_MAX_FRAME_SIZE;

	ret = lan743x_pci_init(adapter, pdev);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "lan743x_pci_init failed, ret = %d", ret);
		goto clean_up;
	}
	adapter->init_flags |= LAN743X_COMPONENT_FLAG_PCI;

	ret = lan743x_csr_init(adapter);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "lan743x_csr_init failed, ret = %d", ret);
		goto clean_up;
	}
	adapter->init_flags |= LAN743X_COMPONENT_FLAG_CSR;

	ret = lan743x_intr_init(adapter);
	if (ret) {
		NETIF_ERROR(adapter, drv, adapter->netdev,
			    "lan743x_intr_init failed, ret = %d", ret);
		goto clean_up;
	}
	adapter->init_flags |= LAN743X_COMPONENT_FLAG_INTR;

	ret = lan743x_dp_init(adapter);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "lan743x_dp_init failed, ret = %d", ret);
		goto clean_up;
	}
	adapter->init_flags |= LAN743X_COMPONENT_FLAG_DP;

	ret = lan743x_gpio_init(adapter);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "lan743x_gpio_init failed, ret = %d", ret);
		goto clean_up;
	}
	adapter->init_flags |= LAN743X_COMPONENT_FLAG_GPIO;

	ret = lan743x_mac_init(adapter);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "lan743x_mac_init failed, ret = %d", ret);
		goto clean_up;
	}
	adapter->init_flags |= LAN743X_COMPONENT_FLAG_MAC;

	ret = lan743x_phy_init(adapter);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "lan743x_phy_init failed, ret = %d", ret);
		goto clean_up;
	}
	adapter->init_flags |= LAN743X_COMPONENT_FLAG_PHY;

	ret = lan743x_ptp_init(adapter);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "lan743x_ptp_init failed, ret = %d", ret);
		goto clean_up;
	}
	adapter->init_flags |= LAN743X_COMPONENT_FLAG_PTP;

	ret = lan743x_rfe_init(adapter);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "lan743x_rfe_init failed, ret = %d", ret);
		goto clean_up;
	}
	adapter->init_flags |= LAN743X_COMPONENT_FLAG_RFE;

	ret = lan743x_fct_init(adapter);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "lan743x_fct_init failed, ret = %d", ret);
		goto clean_up;
	}
	adapter->init_flags |= LAN743X_COMPONENT_FLAG_FCT;

	ret = lan743x_dmac_init(adapter);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "lan743x_dmac_init failed, ret = %d", ret);
		goto clean_up;
	}
	adapter->init_flags |= LAN743X_COMPONENT_FLAG_DMAC;

	ret = lan743x_rx_init(&adapter->rx[0], adapter, 0);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "lan743x_rx_init failed, ret = %d", ret);
		goto clean_up;
	}
	adapter->init_flags |= LAN743X_COMPONENT_FLAG_RX(0);

	ret = lan743x_tx_init(&adapter->tx[0], adapter, 0);
	if (ret) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "lan743x_tx_init failed, ret = %d", ret);
		goto clean_up;
	}
	adapter->init_flags |= LAN743X_COMPONENT_FLAG_TX(0);

	netdev->netdev_ops = &lan743x_netdev_ops;
	netdev->ethtool_ops = &lan743x_ethtool_ops;
	netdev->features = NETIF_F_SG | NETIF_F_TSO | NETIF_F_HW_CSUM;
	netdev->hw_features = netdev->features;

	strncpy(netdev->name, "eth%d", sizeof(netdev->name));
	ret = register_netdev(netdev);
	if (ret < 0) {
		NETIF_ERROR(adapter, probe, adapter->netdev,
			    "failed to register net device, ret = %d", ret);
		goto clean_up;
	}
	adapter->init_flags |= LAN743X_INIT_FLAG_NETDEV_REGISTERED;

	NETIF_INFO(adapter, probe, adapter->netdev, "Probe succeeded");
	ret = 0;

clean_up:
	if (ret && adapter) {
		NETIF_WARNING(adapter, probe, adapter->netdev,
			      "Incomplete initialization, performing clean up");
		lan743x_device_cleanup(adapter);
	}
	return ret;
}

/**
 * lan743x_pcidev_remove - Device Removal Routine
 * @pdev: PCI device information struct
 *
 * this is called by the PCI subsystem to alert the driver
 * that it should release a PCI device.  This could be caused by a
 * Hot-Plug event, or because the driver is going to be removed from
 * memory.
 **/
static void lan743x_pcidev_remove(struct pci_dev *pdev)
{
	struct net_device *netdev = NULL;
	struct lan743x_adapter *adapter = NULL;

	netdev = pci_get_drvdata(pdev);
	adapter = netdev_priv(netdev);
	lan743x_device_cleanup(adapter);
}

static const struct pci_device_id lan743x_pcidev_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_SMSC, PCI_DEVICE_ID_SMSC_LAN7430) },
	{ 0, }
};

static struct pci_driver lan743x_pcidev_driver = {
	.name     = DRIVER_NAME,
	.id_table = lan743x_pcidev_tbl,
	.probe    = lan743x_pcidev_probe,
	.remove   = lan743x_pcidev_remove,
};

static int __init lan743x_module_init(void)
{
	int result = 0;

	pr_info(DRIVER_DESC " %s\n", DRIVER_VERSION);
	pr_info("module parameter\n");
	pr_info("  msg_enable = 0x%04X\n", msg_enable);

	result = pci_register_driver(&lan743x_pcidev_driver);
	if (result)
		pr_warn("pci_register_driver returned error code, %d\n",
			result);
	return result;
}

module_init(lan743x_module_init);

static void __exit lan743x_module_exit(void)
{
	pci_unregister_driver(&lan743x_pcidev_driver);
}

module_exit(lan743x_module_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
