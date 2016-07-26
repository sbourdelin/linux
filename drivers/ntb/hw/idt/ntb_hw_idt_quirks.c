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

/*#define DEBUG*/

#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>

#include "ntb_hw_idt.h"
#include "ntb_hw_idt_quirks.h"

/*
 * Module parameters:
 * @mw_aprt:	Memory Windows aperture (x86: 9 - 26, x64: 9 - 32)
 */
static unsigned char mw_aprt = DEFAULT_MW_APRT;
module_param(mw_aprt, byte, 0000);
MODULE_PARM_DESC(mw_aprt,
	"IDT NTB memory windows aperture. The actual memory windows size is "
	"limited with 2^mw_aprt. It is initially set to 20 so the upper "
	"boundary of the memory windows size would be 1 MB."
	"Both sides, local node and peer MUST set the same value!");

/*
 * Alter the passed driver paremeters
 */
static void idt_ntb_alter_params(struct pci_dev *pdev)
{
	unsigned char mw_aprt_bak = mw_aprt;

	/* Clamp the memory windows aperture parameter */
#ifdef CONFIG_64BIT
	mw_aprt = clamp(mw_aprt, MIN_MW_APRT, MAX_X64_MW_APRT);
#else
	mw_aprt = clamp(mw_aprt, MIN_MW_APRT, MAX_X86_MW_APRT);
#endif /* !CONFIG_64BIT */
	if (mw_aprt_bak != mw_aprt) {
		dev_warn(&pdev->dev,
			"IDT NTB memory windows aperture has been clamped "
			"from %hhu to %hhu", mw_aprt_bak, mw_aprt);
	}

	dev_dbg(&pdev->dev, "IDT NTB HW-driver parameter has been verified");
}

/*
 * IDT PCIe-swtich NTB function BARs pre-initializer
 */
static void idt_ntb_quirks(struct pci_dev *pdev)
{
	int ret;
	u32 lubar_aprt = 0, dirbar_aprt = 0;

	/* Alter the memory windows aperture parameter first */
	idt_ntb_alter_params(pdev);

	/* Calculate memory windows related BAR aperture */
	lubar_aprt = (mw_aprt + MWLUTBL_APRT) << MWAPRT_OFF;
	dirbar_aprt = mw_aprt << MWAPRT_OFF;

	/* Pre-initialize the maximum possible BAR's so don't worry about them
	 * anymore */
	/* BAR0 - Memory mapped Configuration space - x32 Non-prefetchable
	 * memory mapped space. Since it is the registers space then it must be
	 * non-prefetchable, which permits the 32-bits address only according
	 * to the PCI specs. Even though PCIe bridges doesn't do any prefetching
	 * whether prefetch bit is set or not, We'll set the bit as a matter of
	 * legacy */
	ret = pci_write_config_dword(pdev, BARSETUP0_OFF, BARSETUP_CFG_32BIT);
	if (SUCCESS != ret) {
		dev_err(&pdev->dev,
		    "Failed to activate registers configuration space (BAR0)");
		return;
	}

	/* BAR2(+ x64:3) - Memory mapped shared memory with address translation
	 * based on lookup table - x32/x64 Non-prefetchable/prefetchable memory
	 * mapped space with aperture of 2^(mw_aprt + MWLUTBL_APRT), which
	 * effectively gives 2^mw_aprt bytes of memory space per each memory
	 * window */
#ifdef CONFIG_64BIT
	ret = pci_write_config_dword(pdev, BARSETUP2_OFF,
				     BARSETUP_24LUMW_64BIT | lubar_aprt);

#else
	ret = pci_write_config_dword(pdev, BARSETUP2_OFF,
				     BARSETUP_24LUMW_32BIT | lubar_aprt);

#endif /* !CONFIG_64BIT */
	if (SUCCESS != ret) {
		dev_err(&pdev->dev,
		   "Failed to activate lookup table based memory window (BAR2)");
		return;
	}

	/* BAR4(+ x64:5) - Memory mapped shared memory with direct address
	 * translation - x32/x64 Non-prefetchable/prefetchable memory
	 * mapped space with aperture of 2^(mw_aprt + MWLUTBL_APRT) */
#ifdef CONFIG_64BIT
	ret = pci_write_config_dword(pdev, BARSETUP4_OFF,
				     BARSETUP_DIRMW_64BIT | dirbar_aprt);

#else
	ret = pci_write_config_dword(pdev, BARSETUP4_OFF,
				     BARSETUP_DIRMW_32BIT | dirbar_aprt);

#endif /* !CONFIG_64BIT */
	if (SUCCESS != ret) {
		dev_err(&pdev->dev,
		    "Failed to activate directly mapped memory window (BAR4)");
		return;
	}

	dev_dbg(&pdev->dev, "IDT NTB BAR's enabled");
}
IDT_NTB_PCI_FIXUP_EARLY(89HPES24NT6AG2,  idt_ntb_quirks);
IDT_NTB_PCI_FIXUP_EARLY(89HPES32NT8AG2,  idt_ntb_quirks);
IDT_NTB_PCI_FIXUP_EARLY(89HPES32NT8BG2,  idt_ntb_quirks);
IDT_NTB_PCI_FIXUP_EARLY(89HPES12NT12G2,  idt_ntb_quirks);
IDT_NTB_PCI_FIXUP_EARLY(89HPES16NT16G2,  idt_ntb_quirks);
IDT_NTB_PCI_FIXUP_EARLY(89HPES24NT24G2,  idt_ntb_quirks);
IDT_NTB_PCI_FIXUP_EARLY(89HPES32NT24AG2, idt_ntb_quirks);
IDT_NTB_PCI_FIXUP_EARLY(89HPES32NT24BG2, idt_ntb_quirks);

