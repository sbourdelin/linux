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

#ifndef NTB_HW_IDT_QUIRKS_H
#define NTB_HW_IDT_QUIRKS_H

#include <linux/pci.h>
#include <linux/pci_ids.h>

#include "ntb_hw_idt.h"

/*
 * Macro is used to create the struct pci_fixup that matches the supported
 * IDT PCIe-switches
 * @devname:	Capitalized name of the particular device
 * @hook:	Fixup hook function name
 */
#define IDT_NTB_PCI_FIXUP_EARLY(devname, hook) \
	DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_IDT, \
		PCI_DEVICE_ID_IDT_##devname, PCI_CLASS_BRIDGE_OTHER, 8U, hook)

/*
 * IDT PCIe-switch NTB function BAR setup parameters:
 * @BARSETUP{N}_OFF:		BAR{N} setup register offset
 * @BARSETUP_CFG_32BIT:		32-bits addressable non-prefetchable memory
 *				mapped registers configuration space
 * @BARSETUP_CFG_64BIT:		64-bits addressable prefetchable memory
 *				mapped registers configuration space
 * @BARSETUP_DIRMW_32BIT:	32-bits addresable non-prefetchable direct
 *				address translatable memory window
 * @BARSETUP_DIRMW_64BIT:	64-bits addresable prefetchable direct
 *				address translatable memory window
 * @BARSETUP_12LUMW_32BIT:	32-bits addresable non-prefetchable 12-entries
 *				lookup table memory window
 * @BARSETUP_12LUMW_64BIT:	64-bits addresable prefetchable 12-entries
 *				lookup table memory window
 * @BARSETUP_24LUMW_32BIT:	32-bits addresable non-prefetchable 24-entries
 *				lookup table memory window
 * @BARSETUP_24LUMW_64BIT:	64-bits addresable prefetchable 24-entries
 *				lookup table memory window
 *
 */
#define BARSETUP0_OFF 0x00470
#define BARSETUP1_OFF 0x00480
#define BARSETUP2_OFF 0x00490
#define BARSETUP3_OFF 0x004A0
#define BARSETUP4_OFF 0x004B0
#define BARSETUP5_OFF 0x004C0
#define BARSETUP_CFG_32BIT ((u32)0x800004C0U)
#define BARSETUP_CFG_64BIT ((u32)0x800004CCU)
#define BARSETUP_DIRMW_32BIT ((u32)0x80000000U)
#define BARSETUP_DIRMW_64BIT ((u32)0x8000000CU)
#define BARSETUP_12LUMW_32BIT ((u32)0x80000800U)
#define BARSETUP_12LUMW_64BIT ((u32)0x8000080CU)
#define BARSETUP_24LUMW_32BIT ((u32)0x80001000U)
#define BARSETUP_24LUMW_64BIT ((u32)0x8000100CU)
#define MWAPRT_OFF 4

/*
 * IDT PCIe-switch NTB function related parameters:
 * @DEFAULT_MW_APRT:		Default aperture of the memory windows (that is
 *				maximum size of the memory windows)
 * @MIN_MW_APRT:		Minimum possible aperture of the memory windows
 * @MAX_X86_MW_APRT:		Maximum aperture for x86 architecture
 * @MAX_X64_MW_APRT:		Maximum aperture for x64 architecture
 * @MWLUTBL_APRT:		Additional value to translate the per memory
 *				windows specific aperture to the aperture of
 *				the whole lookup table
 */
#define DEFAULT_MW_APRT (unsigned char)20
#define MIN_MW_APRT (unsigned char)9
#define MAX_X86_MW_APRT (unsigned char)26
#define MAX_X64_MW_APRT (unsigned char)32
#define MWLUTBL_APRT (unsigned char)5

#endif /* NTB_HW_IDT_QUIRKS_H */
