/*
 *   This file is provided under a GPLv2 license.  When using or
 *   redistributing this file, you may do so under that license.
 *
 *   GPL LICENSE SUMMARY
 *
 *   Copyright (C) 2016 T-Platforms All Rights Reserved.
 *
 *   This program is free software; you can redistribute it and/or modify it
 *   under the terms and conditions of the GNU General Public License,
 *   version 2, as published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful, but WITHOUT
 *   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *   FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 *   more details.
 *
 *   You should have received a copy of the GNU General Public License along with
 *   this program; if not, one can be found <http://www.gnu.org/licenses/>.
 *
 *   The full GNU General Public License is included in this distribution in
 *   the file called "COPYING".
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * IDT PCIe-switch NTB Linux driver
 *
 * Contact Information:
 * Serge Semin <fancer.lancer@gmail.com>, <Sergey.Semin@t-platforms.ru>
 */

#ifndef NTB_HW_IDT_H
#define NTB_HW_IDT_H

#include <linux/types.h>
#include <linux/ntb.h>
#include <linux/pci.h>
#include <linux/pci_ids.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/list.h>

#include "ntb_hw_idt_regmap.h"

/*
 * Macro is used to create the struct pci_device_id that matches
 * the supported IDT PCIe-switches
 * @devname: Capitalized name of the particular device
 * @data: Variable passed to the driver of the particular device
 */
#define IDT_PCI_DEVICE_IDS(devname, data) \
	.vendor = PCI_VENDOR_ID_IDT, .device = PCI_DEVICE_ID_IDT_##devname, \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, \
	.class = (PCI_CLASS_BRIDGE_OTHER << 8), .class_mask = (0xFFFF00), \
	.driver_data = (kernel_ulong_t)&data

/*
 * IDT PCIe-switches device IDs
 */
#define PCI_DEVICE_ID_IDT_89HPES24NT6AG2 0x8091
#define PCI_DEVICE_ID_IDT_89HPES32NT8AG2 0x808F
#define PCI_DEVICE_ID_IDT_89HPES32NT8BG2 0x8088
#define PCI_DEVICE_ID_IDT_89HPES12NT12G2 0x8092
#define PCI_DEVICE_ID_IDT_89HPES16NT16G2 0x8090
#define PCI_DEVICE_ID_IDT_89HPES24NT24G2 0x808E
#define PCI_DEVICE_ID_IDT_89HPES32NT24AG2 0x808C
#define PCI_DEVICE_ID_IDT_89HPES32NT24BG2 0x808A

/*
 * Some common constant used in the driver for better readability:
 * @ON:	Enable something
 * @OFF: Disable something
 * @SUCCESS: Success of a function execution
 * @BAR0: Operation with BAR0
 * @BAR2: Operation with BAR2
 * @BAR4: Operation with BAR4
 */
#define ON ((u32)0x1)
#define OFF ((u32)0x0)
#define SUCCESS 0
#define BAR0 0
#define BAR2 2
#define BAR4 4

/*
 * Inline helper function to perform the for each set bit looping.
 *
 * NOTE We don't use the standard for_each_set_bit because it's unsigned
 *      long aligned, but our registers are u32 sized.
 */
static __always_inline int next_bit(u32 var, int bit)
{
	int pos;

	pos = ffs(var & ~(((u32)1 << bit) - 1));
	return (0 == pos || 32 <= bit) ? 32 : (pos - 1);
}

/*
 * Perform loop for each set bit of a u32 variable.
 *
 * NOTE Size of integer is supposed to be 32-bits or greater so this
 *      "for each"-macro would work.
 */
#define for_each_set_bit_u32(var, bit) \
	for ((bit) = next_bit(var, 0); \
	     bit < 32; \
	     (bit) = next_bit(var, (bit) + 1))

/*
 * Create a contiguous bitmask starting at bit position @l and ending at
 * position @h. For example
 * GENMASK_ULL(39, 21) gives us the 64bit vector 0x000000ffffe00000.
 */
#ifndef GENMASK
#define GENMASK(h, l) \
		(((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#endif /* !GENMASK */

/*
 * Number of NTB resource like Doorbell bits, Memory windows
 * and Message registers
 */
#define IDT_NTB_DBELL_CNT 32
#define IDT_NTB_MW_CNT 24
#define IDT_NTB_MSG_CNT 4
#define IDT_NTB_MTBL_ENTRY_CNT 64

/*
 * General IDT PCIe-switch constant
 * @IDT_NTB_MAXPORTS_CNT:	Maximum number of ports per IDT PCIe-switch
 * @IDT_NTB_MAXPARTS_CNT:	Maximum number of partitions per IDT PCIe-switch
 * @IDT_PCIE_REGSIZE:		Size of the registers in bytes
 * @IDT_NTB_TRANSALIGN:		Alignment of the translated base address
 * @IDT_NTB_LNKPOLL_TOUT:	Timeout of the link polling kernel thread
 * @IDT_NTB_SENDMSG_TOUT:	Timeout of sending the next message to a peer
 * @IDT_NTB_TEMP_LTH:		Lower threshold of the IDT temperature sensor
 * @IDT_NTB_TEMP_HTH:		Higher threshold of the IDT temperature sensor
 */
#define IDT_NTB_MAXPORTS_CNT 24
#define IDT_NTB_MAXPARTS_CNT 8
#define IDT_PCIE_REGSIZE 4
#define IDT_NTB_TRANSALIGN 4
#define IDT_NTB_LNKPOLL_TOUT msecs_to_jiffies(1000)
#define IDT_NTB_SENDMSG_TOUT msecs_to_jiffies(100)
#define IDT_NTB_SENDMSG_RETRY 50
#define IDT_NTB_TEMP_LTH (u32)10
#define IDT_NTB_TEMP_HTH (u32)85

/*
 * u32 data atomic structure
 */
typedef struct {
	spinlock_t lock;
	u32 data;
} atomic_u32_t;

/*
 * Queue head with atomic access
 */
typedef struct {
	struct list_head head;
	spinlock_t lock;
} queue_atomic_t;

/*
 * Messages list container
 * @msg:	Message structure
 * @retry:	Number of retries left
 * @entry:	Queue entry
 */
struct idt_ntb_msg {
	struct ntb_msg msg;
	int retry;
	struct list_head entry;
};
#define to_msg_list_entry(pentry) \
	(list_entry(pentry, struct idt_ntb_msg, entry))

/*
 * IDT PCIe-switch model private data
 * @port_cnt:	Total number of NT endpoint ports
 * @ports:	Port ids
 */
struct idt_89hpes_pdata {
	unsigned char port_cnt;
	unsigned char ports[];
};

/*
 * NTB-bus device structure
 * @ntb:		NTB-bus device related structure
 *
 * @port:		Remote NT-function port
 * @part:		Remote NT-function partition
 *
 * @pairid:		Global Identifier of Primary-Secondary ports pair
 *
 * @lnk_sts:		Peer side link status
 *
 * @mw_self_cnt:	Number of memory windows locally available
 * @mw_self_offset:	Offset of the first memory window in the Lookup table
 * @mw_peer_cnt:	Number of peer memory windows
 *
 * @db_cnt:		Number of Doorbells for communications with the
 *			peer NT-function
 * @db_self_offset:	Bits offset of the self Doorbells
 * @db_peer_offset:	Bits offset of the peer Doorbells
 * @db_valid_mask:	Doorbell valid mask
 * @db_self_mask:	Mask of the shifted by self_offset doorbells
 * @db_peer_mask:	Mask of the shifted by peer_offset doorbells
 *
 * @qinmsg:		Queue of inbound messages received from the peer
 * @inmsg_work:		Work thread rising event of new message arrival
 * @qoutmsg:		Queue of outbound messages posted to send to the peer
 * @outmsg_work:	Work thread sending messages
 */
struct idt_ntb_dev {
	struct ntb_dev ntb;

	unsigned char port;
	unsigned char part;

	unsigned char pairid;

	u32 lnk_sts;

	unsigned char mw_self_cnt;
	unsigned char mw_self_offset;
	unsigned char mw_peer_cnt;

	unsigned char db_cnt;
	unsigned char db_self_offset;
	unsigned char db_peer_offset;
	u32 db_valid_mask;
	u32 db_self_mask;
	u32 db_peer_mask;

	queue_atomic_t qinmsg;
	struct work_struct inmsg_work;
	queue_atomic_t qoutmsg;
	struct delayed_work outmsg_work;
};
#define to_ndev_ntb(pntb) container_of(pntb, struct idt_ntb_dev, ntb)
#define to_pdev_ndev(ndev) ((ndev)->ntb.pdev)
#define to_dev_ndev(ndev) (&ndev->ntb.dev)
#define to_data_ndev(ndev) \
	((struct idt_ntb_data *)(pci_get_drvdata(to_pdev_ndev(ndev))))
#define to_cfg_ndev(ndev) (to_data_ndev(ndev)->cfg_mmio)
#define to_ndev_inmsg_work(work) \
	container_of(work, struct idt_ntb_dev, inmsg_work)
#define to_ndev_outmsg_work(work) \
	container_of(to_delayed_work(work), struct idt_ntb_dev, outmsg_work)

/*
 * IDT PCIe-switch NTB bus topology structure
 * @paircnt: Total number of the NTB pair in the current topology
 *           (it's just the number of Secondary ports)
 *
 * @priports: Bitset of Primary ports
 * @secports: Array of Secondary ports bitsets related to the corresponding
 *            Primary ports
 */
struct idt_ntb_topo {
	unsigned char paircnt;

	u32 priports;
	u32 secports[IDT_NTB_MAXPORTS_CNT];
};

/*
 * Structure related to the local IDT PCIe-switch NT-function
 * @pdev:	Pointer to the PCI-bus device
 * @swcfg:	Pointer to the struct idt_89hpes_pdata related to the current
 *		IDT PCIe-switch
 *
 * @port:	Local NT-function port
 * @part:	Local NT-function partition
 *
 * @topo:	Topology of the NT-function ports
 * @role:	Local port role in the IDT topology
 *
 * @peer_cnt:	Number of possible remote peers
 * @ndevs:	Array of the device-related structures
 *
 * @cfg_mmio:	Virtual address of the memory mapped configuration space
 *		of the NT-function
 *
 * @idt_wq:	IDT driver workqueue to setup the link poll and messages
 *		delivery operations
 *
 * @lnk_work:	Link status polling kernel thread
 *
 * @mw_base:	Physical address of the memory mapped base address of the
 *		Memory Windows
 * @mw_size:	Size of one Memory Window
 * @lut_lock:	Lookup table access spin lock
 *
 * @db_sts:	Doorbell status atomic variable
 * @db_msk:	Doorbell mask atomic variable
 * @db_lock:	Doorbell status and mask spin lock
 * @db_tasklet:	Tasklet to handle the doorbell events
 *
 * @msg_lock:		Messages routing table lock
 * @msg_cache:		Slab cache of the message structures
 * @msg_tasklet:	Tasklet - handler of the incoming messages
 *
 * @dbgfs_dir:	DebugFS directory to place the driver debug file
 */
struct idt_ntb_data {
	struct pci_dev *pdev;
	struct idt_89hpes_pdata *swcfg;

	unsigned char port;
	unsigned char part;

	struct idt_ntb_topo topo;
	enum ntb_topo role;

	unsigned char peer_cnt;
	struct idt_ntb_dev *ndevs;

	void __iomem *cfg_mmio;

	struct workqueue_struct *idt_wq;

	struct delayed_work lnk_work;

	phys_addr_t mw_base;
	resource_size_t mw_size;
	spinlock_t lut_lock;

	u32 db_sts;
	u32 db_msk;
	spinlock_t db_lock;
	struct tasklet_struct db_tasklet;

	spinlock_t msg_lock;
	struct kmem_cache *msg_cache;
	struct tasklet_struct msg_tasklet;

	struct dentry *dbgfs_dir;
};
#define to_dev_data(data) (&(data->pdev->dev))
#define to_data_lnkwork(work) \
	container_of(to_delayed_work(work), struct idt_ntb_data, lnk_work)

/*
 * Descriptor of the IDT PCIe-switch port specific parameters in the
 * Global Configuration Space
 * @pcicmd:	PCI command register
 * @pcielsts:	PCIe link status
 * @ntsdata:	NT signal data
 * @ntgsignal:	NT global signal
 *
 * @ctl:	Port control register
 * @sts:	Port status register
 */
struct idt_ntb_port {
	enum idt_ntb_cfgreg pcicmd;
	enum idt_ntb_cfgreg pcielsts;
	enum idt_ntb_cfgreg ntsdata;
	enum idt_ntb_cfgreg ntgsignal;

	enum idt_ntb_cfgreg ctl;
	enum idt_ntb_cfgreg sts;
};

/*
 * Descriptor of the IDT PCIe-switch partition specific parameters.
 * @ctl: Partition control register in the Global Address Space
 * @sts: Partition status register in the Global Address Space
 */
struct idt_ntb_part {
	enum idt_ntb_cfgreg ctl;
	enum idt_ntb_cfgreg sts;
	enum idt_ntb_cfgreg msgctl[IDT_NTB_MSG_CNT];
};

#endif /* NTB_HW_IDT_H */
