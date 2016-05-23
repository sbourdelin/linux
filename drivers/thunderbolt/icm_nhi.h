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

#ifndef ICM_NHI_H_
#define ICM_NHI_H_

#include <linux/pci.h>

#define DEVICE_DATA(num_ports, dma_port, nvm_ver_offset, nvm_auth_on_boot,\
		    support_full_e2e) \
	((num_ports) | ((dma_port) << 4) | ((nvm_ver_offset) << 10) | \
	 ((nvm_auth_on_boot) << 22) | ((support_full_e2e) << 23))
#define DEVICE_DATA_ICM_CAPABLITY(driver_data) ((driver_data) != 0)
#define DEVICE_DATA_NUM_PORTS(device_data) ((device_data) & 0xf)
#define DEVICE_DATA_DMA_PORT(device_data) (((device_data) >> 4) & 0x3f)
#define DEVICE_DATA_NVM_VER_OFFSET(device_data) (((device_data) >> 10) & 0xfff)
#define DEVICE_DATA_NVM_AUTH_ON_BOOT(device_data) (((device_data) >> 22) & 0x1)
#define DEVICE_DATA_SUPPORT_FULL_E2E(device_data) (((device_data) >> 23) & 0x1)

int nhi_genl_register(void);
int nhi_genl_unregister(void);
int icm_nhi_init(struct pci_dev *pdev,
		 const struct pci_device_id *id,
		 void __iomem *iobase);
void icm_nhi_deinit(struct pci_dev *pdev);
int nhi_suspend(struct device *dev) __releases(&nhi_ctxt->send_sem);
int nhi_resume(struct device *dev) __acquires(&nhi_ctxt->send_sem);
void icm_nhi_shutdown(struct pci_dev *pdev);

#endif
