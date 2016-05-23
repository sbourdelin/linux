/*******************************************************************************
 *
 * Intel Thunderbolt(TM) driver
 * Copyright(c) 2014 - 2016 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Intel Thunderbolt Mailing List <thunderbolt-software@lists.01.org>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 ******************************************************************************/

#ifndef NET_H_
#define NET_H_

#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <net/genetlink.h>

/*
 * Each physical port contains 2 channels.
 * Devices are exposed to user based on physical ports.
 */
#define CHANNELS_PER_PORT_NUM 2
/*
 * Calculate host physical port number (Zero-based numbering) from
 * host channel/link which starts from 1.
 */
#define PORT_NUM_FROM_LINK(link) (((link) - 1) / CHANNELS_PER_PORT_NUM)

#define TBT_TX_RING_FULL(prod, cons, size) ((((prod) + 1) % (size)) == (cons))
#define TBT_TX_RING_EMPTY(prod, cons) ((prod) == (cons))
#define TBT_RX_RING_FULL(prod, cons) ((prod) == (cons))
#define TBT_RX_RING_EMPTY(prod, cons, size) ((((cons) + 1) % (size)) == (prod))

#define PATH_FROM_PORT(num_paths, port_num) (((num_paths) - 1) - (port_num))

/* PDF values for SW<->FW communication in raw mode */
enum pdf_value {
	PDF_READ_CONFIGURATION_REGISTERS = 1,
	PDF_WRITE_CONFIGURATION_REGISTERS,
	PDF_ERROR_NOTIFICATION,
	PDF_ERROR_ACKNOWLEDGMENT,
	PDF_PLUG_EVENT_NOTIFICATION,
	PDF_INTER_DOMAIN_REQUEST,
	PDF_INTER_DOMAIN_RESPONSE,
	PDF_CM_OVERRIDE,
	PDF_RESET_CIO_SWITCH,
	PDF_FW_TO_SW_NOTIFICATION,
	PDF_SW_TO_FW_COMMAND,
	PDF_FW_TO_SW_RESPONSE
};

/*
 * SW->FW commands
 * CC = Command Code
 */
enum {
	CC_GET_THUNDERBOLT_TOPOLOGY = 1,
	CC_GET_VIDEO_RESOURCES_DATA,
	CC_DRV_READY,
	CC_APPROVE_PCI_CONNECTION,
	CC_CHALLENGE_PCI_CONNECTION,
	CC_ADD_DEVICE_AND_KEY,
	CC_APPROVE_INTER_DOMAIN_CONNECTION = 0x10
};

/*
 * SW -> FW mailbox commands
 * CC = Command Code
 */
enum {
	CC_STOP_CM_ACTIVITY,
	CC_ENTER_PASS_THROUGH_MODE,
	CC_ENTER_CM_OWNERSHIP_MODE,
	CC_DRV_LOADED,
	CC_DRV_UNLOADED,
	CC_SAVE_CURRENT_CONNECTED_DEVICES,
	CC_DISCONNECT_PCIE_PATHS,
	CC_DRV_UNLOADS_AND_DISCONNECT_INTER_DOMAIN_PATHS,
	DISCONNECT_PORT_A_INTER_DOMAIN_PATH = 0x10,
	DISCONNECT_PORT_B_INTER_DOMAIN_PATH,
	DP_TUNNEL_MODE_IN_ORDER_PER_CAPABILITIES = 0x1E,
	DP_TUNNEL_MODE_MAXIMIZE_SNK_SRC_TUNNELS,
	CC_SET_FW_MODE_FD1_D1_CERT = 0x20,
	CC_SET_FW_MODE_FD1_D1_ALL,
	CC_SET_FW_MODE_FD1_DA_CERT,
	CC_SET_FW_MODE_FD1_DA_ALL,
	CC_SET_FW_MODE_FDA_D1_CERT,
	CC_SET_FW_MODE_FDA_D1_ALL,
	CC_SET_FW_MODE_FDA_DA_CERT,
	CC_SET_FW_MODE_FDA_DA_ALL
};


/* NHI genetlink attributes */
enum {
	NHI_ATTR_UNSPEC,
	NHI_ATTR_DRV_VERSION,
	NHI_ATTR_NVM_VER_OFFSET,
	NHI_ATTR_NUM_PORTS,
	NHI_ATTR_DMA_PORT,
	NHI_ATTR_SUPPORT_FULL_E2E,
	NHI_ATTR_MAILBOX_CMD,
	NHI_ATTR_PDF,
	NHI_ATTR_MSG_TO_ICM,
	NHI_ATTR_MSG_FROM_ICM,
	__NHI_ATTR_MAX,
};
#define NHI_ATTR_MAX (__NHI_ATTR_MAX - 1)

struct port_net_dev {
	struct net_device *net_dev;
	struct mutex state_mutex;
};

/**
 *  struct tbt_nhi_ctxt - thunderbolt native host interface context
 *  @icm_enabled:			distinguish if iCM enabled system.
 *  @d0_exit:				whether controller exit D0 state.
 *  @node:				node in the controllers list.
 *  @pdev:				pci device information.
 *  @iobase:				address of I/O.
 *  @msix_entries:			MSI-X vectors.
 *  @icm_ring_shared_mem:		virtual address of iCM ring.
 *  @icm_ring_shared_mem_dma_addr:	DMA addr of iCM ring.
 *  @send_sem:				semaphore for sending messages to iCM
 *					one at a time.
 *  @mailbox_mutex:			mutex for sending mailbox commands to
 *					iCM one at a time.
 *  @d0_exit_send_mutex:		synchronizing the d0 exit with messages.
 *  @d0_exit_mailbox_mutex:		synchronizing the d0 exit with mailbox.
 *  @lock:				synchronizing the interrupt registers
 *					access.
 *  @icm_msgs_work:			work queue for handling messages
 *					from iCM.
 *  @net_devices:			net devices per port.
 *  @net_workqueue:			work queue to send net messages.
 *  @id:				id of the controller.
 *  @num_paths:				number of paths supported by controller.
 *  @nvm_ver_offset:			offset of NVM version in NVM.
 *  @num_vectors:			number of MSI-X vectors.
 *  @num_ports:				number of ports in the controller.
 *  @dma_port:				DMA port.
 *  @nvm_auth_on_boot:			whether iCM authenticates the NVM
 *					during boot.
 *  @wait_for_icm_resp:			whether to wait for iCM response.
 *  @ignore_icm_resp:			whether to ignore iCM response.
 *  @pci_using_dac:			whether using DAC.
 *  @support_full_e2e:			whether controller support full E2E.
 */
struct tbt_nhi_ctxt {
	bool icm_enabled;	/* icm_enabled must be the first field */
	bool d0_exit;
	struct list_head node;
	struct pci_dev *pdev;
	void __iomem *iobase;
	struct msix_entry *msix_entries;
	struct tbt_icm_ring_shared_memory *icm_ring_shared_mem;
	dma_addr_t icm_ring_shared_mem_dma_addr;
	struct semaphore send_sem;
	struct mutex mailbox_mutex;
	struct mutex d0_exit_send_mutex;
	struct mutex d0_exit_mailbox_mutex;
	spinlock_t lock;
	struct work_struct icm_msgs_work;
	struct port_net_dev *net_devices;
	struct workqueue_struct *net_workqueue;
	u32 id;
	u32 num_paths;
	u16 nvm_ver_offset;
	u8 num_vectors;
	u8 num_ports;
	u8 dma_port;
	bool nvm_auth_on_boot : 1;
	bool wait_for_icm_resp : 1;
	bool ignore_icm_resp : 1;
	bool pci_using_dac : 1;
	bool support_full_e2e : 1;
};

#endif
