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

#ifndef NTB_HW_IDT_REGMAP_H
#define NTB_HW_IDT_REGMAP_H

#include <linux/compiler.h>
#include <linux/spinlock.h>

/*
 * Helper macros to enumerate the the registers and fields tables
 * identifications.
 * It's used in conjunction with the IDT_NT_REGFLDS(), IDT_SW_REGFLDS(),
 * IDT_NT_CFGREGS() and IDT_SW_CFGREGS() macroses
 */
#define PAIR_ID_ENUM(ID, reg, mask, offset) ID,

/*
 * Helper macros to pair the Field identification with the corresponding
 * register, mask and the offset
 * It's used in conjunction with the IDT_NT_REGFLDS() and
 * IDT_SW_REGFLDS() macroses
 */
#define PAIR_FLDID_ACCESS(ID, reg, mask, offset, retreg, retmask, retoffset) \
	case ID: \
		retreg = reg; \
		retmask = mask; \
		retoffset = offset; \
		break;

/*
 * Helper macros to pair the registers identifications with the corresponding
 * offset and the size
 * It's used in conjunction with the IDT_NT_CFGREGS() and
 * IDT_SW_CFGREGS() macroses
 */
#define PAIR_REGID_ACCESS(ID, addr, size, desc, retaddr, retsize, retdesc) \
	case ID: \
		retaddr = addr; \
		retsize = size; \
		retdesc = desc; \
		break;

/*
 * IDT PCIe-swtich registers related constants:
 *
 * @BARSTP_MEMMAP:	Memory mapped BAR select
 * @BARSTP_TYPE_32:	32-bits addressable BAR select
 * @BARSTP_TYPE_64:	64-bits addressable BAR select
 * @BARSTP_NONPREF:	Non-prefetchable memory
 * @BARSTP_PREF:	Prefetchable memory
 * @BARSTP_MINSIZE:	Minimum BAR aperture (2^SIZE) for Lookup table
 * @BARSTP_MAXSIZE_32:	Maximum BAR aperture for Lookup table and x86 CPU
 * @BARSTP_MAXSIZE_64:	Maximum BAR aperture for Lookup table and x64 CPU
 * @BARSTP_MODE_WNDW:	Memory Window mode of BAR
 * @BARSTP_MODE_CFGSPC:	Configuration space mode of BAR
 * @BARSTP_ATRAN_DRCT:	Direct address translation
 * @BARSTP_ATRAN_LU12:	12-entries Lookup table for address translation
 * @BARSTP_ATRAN_LU24:	24-entries Lookup table for address translation
 *
 * @GASAADDR_OFFSET:	GASAADDR register offset in the NT-function config space
 * @GASADATA_OFFSET:	GASADATA register offset in the NT-function config space
 *
 * @PORTMODE_NT:	Port mode - just one NT function
 * @PORTMODE_USNT:	Port mode - upstream switch port with NT function
 * @PORTMODE_USNTDMA:	Port mode - upstream switch port with NT and DMA
 *			functions
 *
 * @NTINT_MASK:		Mask the NT interrupts (Msg, DB, SE, Temp)
 * @NTINT_UNMASK:	Unmask the NT interrupts (Msg, DB, SE, Temp)
 * @ALLINT_MASK:	Mask all the available interrupts
 * @ALLINT_UNMASK:	Unmask all the available unterrupts
 * @MSGINT_BIT:		Message interrupt serial bit
 * @DBINT_BIT:		Doorbell interrupt serial bit
 * @SEINT_BIT:		Switch events interrupt serial bit
 * @FMCI_BIT:		Failover mode change initiated
 * @FMCC_BIT:		Failover mode change completed
 * @TEMPINT_BIT:	Temperature sensor interrupt serial bit
 *
 * @INDB_MASK:		Mask the inbound doorbells interrupt
 * @INDB_UNMASK:	Unmask the inbound doorbells interrupt
 * @OUTMSG_MASK:	Mask all the outbound message bits
 * @INMSG_MASK:		Mask all the inbound message bits
 * @INMSG_STS:		Valid inbound message status
 * @MSG_MASK:		Mask all the message interrupts
 * @MSG_UNMASK:		Unmask the first inbound message register interrupt
 */
#define BARSTP_MEMMAP ((u32)0x0)
#define BARSTP_TYPE_32 ((u32)0x0)
#define BARSTP_TYPE_64 ((u32)0x2)
#define BARSTP_NONPREF ((u32)0x0)
#define BARSTP_PREF ((u32)0x1)
#define BARSTP_MINSIZE ((u32)14)
#define BARSTP_MAXSIZE_32 ((u32)16)
#define BARSTP_MAXSIZE_64 ((u32)37)
#define BARSTP_MODE_WNDW ((u32)0x0)
#define BARSTP_MODE_CFGSPC ((u32)0x1)
#define BARSTP_ATRAN_DRCT ((u32)0x0)
#define BARSTP_ATRAN_LU12 ((u32)0x1)
#define BARSTP_ATRAN_LU24 ((u32)0x2)
#define GASAADDR_OFFSET ((ptrdiff_t)0x00FF8)
#define GASADATA_OFFSET ((ptrdiff_t)0x00FFC)
#define PORTMODE_NT ((u32)0x3)
#define PORTMODE_USNT ((u32)0x4)
#define PORTMODE_USNTDMA ((u32)0x7)
#define NTINT_MASK ((u32)0x8B)
#define NTINT_UNMASK (~NTINT_MASK)
#define ALLINT_MASK ((u32)0xBB)
#define ALLINT_UNMASK (~ALLINT_MASK)
#define MSGINT_BIT ((u32)0)
#define DBINT_BIT ((u32)1)
#define SEINT_BIT ((u32)3)
#define FMCI_BIT ((u32)4)
#define FMCC_BIT ((u32)5)
#define TEMPINT_BIT ((u32)7)
#define INDB_MASK ((u32)-1)
#define INDB_UNMASK ((u32)0x0)
#define OUTMSG_MASK ((u32)0xF)
#define INMSG_MASK ((u32)0xF0000)
#define INMSG_STS ((u32)0xF)
#define MSG_MASK ((u32)0xF000F)
#define MSG_UNMASK ((u32)0xE000F)

/*
 * Table of the register fields accessed over either the NT-function
 * memory mapped registers or IDT PCIe-switch Global registers.
 * This table is then translated into the switch-case statement
 * so to get the proper "Name"->{reg addr, mask, fld offset}
 * pairs.
 */
#define IDT_NT_REGFLDS(X, args...) \
	/* PCI command register */ \
	X(IDT_NT_IOAE,          IDT_NT_PCI_CMD, 0x1, 0, ## args) \
	X(IDT_NT_MAE,           IDT_NT_PCI_CMD, 0x1, 1, ## args) \
	X(IDT_NT_BME,           IDT_NT_PCI_CMD, 0x1, 2, ## args) \
	/* Link status registers */ \
	X(IDT_NT_MAXLNKSPD,     IDT_NT_PCI_PCIELCAP, 0xF, 0, ## args) \
	X(IDT_NT_MAXLNKWDTH,    IDT_NT_PCI_PCIELCAP, 0x3F, 4, ## args) \
	X(IDT_NT_PORTNUM,       IDT_NT_PCI_PCIELCAP, 0xFF, 24, ## args) \
	X(IDT_NT_CURLNKSPD,     IDT_NT_PCI_PCIELSTS, 0xF, 0, ## args) \
	X(IDT_NT_CURLNKWDTH,    IDT_NT_PCI_PCIELSTS, 0x3F, 4, ## args) \
	X(IDT_NT_SCLK,          IDT_NT_PCI_PCIELSTS, 0x1, 12, ## args) \
	/* SSVID/SSID registers */\
	X(IDT_NT_SSVID,         IDT_NT_PCI_SSIDSSVID, 0xFFFF, 0, ## args) \
	X(IDT_NT_SSID,          IDT_NT_PCI_SSIDSSVID, 0xFFFF, 16, ## args) \
	/* General NT-function registers */\
	X(IDT_NT_IDPROTDIS,     IDT_NT_PCI_NTCTL, 0x1, 0, ## args) \
	X(IDT_NT_CPEN,          IDT_NT_PCI_NTCTL, 0x1, 1, ## args) \
	/* NT interrupts related registers */ \
	X(IDT_NT_INTSTS,        IDT_NT_PCI_NTINTSTS, 0xBB, 0, ## args) \
	X(IDT_NT_MSGINT_STS,    IDT_NT_PCI_NTINTSTS, 0x1, 0, ## args) \
	X(IDT_NT_DBINT_STS,     IDT_NT_PCI_NTINTSTS, 0x1, 1, ## args) \
	X(IDT_NT_SEINT_STS,     IDT_NT_PCI_NTINTSTS, 0x1, 3, ## args) \
	X(IDT_NT_FMCIINT_STS,   IDT_NT_PCI_NTINTSTS, 0x1, 4, ## args) \
	X(IDT_NT_FMCCINT_STS,   IDT_NT_PCI_NTINTSTS, 0x1, 5, ## args) \
	X(IDT_NT_TMPINT_STS,    IDT_NT_PCI_NTINTSTS, 0x1, 7, ## args) \
	X(IDT_NT_INTMSK,        IDT_NT_PCI_NTINTMSK, 0xBB, 0, ## args) \
	X(IDT_NT_MSGINT_MSK,    IDT_NT_PCI_NTINTMSK, 0x1, 0, ## args) \
	X(IDT_NT_DBINT_MSK,     IDT_NT_PCI_NTINTMSK, 0x1, 1, ## args) \
	X(IDT_NT_SEINT_MSK,     IDT_NT_PCI_NTINTMSK, 0x1, 3, ## args) \
	X(IDT_NT_FMCIINT_MSK,   IDT_NT_PCI_NTINTMSK, 0x1, 4, ## args) \
	X(IDT_NT_FMCCINT_MSK,   IDT_NT_PCI_NTINTMSK, 0x1, 5, ## args) \
	X(IDT_NT_TMPINT_MSK,    IDT_NT_PCI_NTINTMSK, 0x1, 7, ## args) \
	X(IDT_NT_GSIGNAL,       IDT_NT_PCI_NTGSIGNAL, 0x1, 0, ## args) \
	/* Message registers status and masks */ \
	X(IDT_NT_OUTMSGSTS,     IDT_NT_PCI_MSGSTS, 0xF, 0, ## args) \
	X(IDT_NT_INMSGSTS,      IDT_NT_PCI_MSGSTS, 0xF, 16, ## args) \
	X(IDT_NT_OUTMSG0STSMSK, IDT_NT_PCI_MSGSTSMSK, 0x1, 0, ## args) \
	X(IDT_NT_OUTMSG1STSMSK, IDT_NT_PCI_MSGSTSMSK, 0x1, 1, ## args) \
	X(IDT_NT_OUTMSG2STSMSK, IDT_NT_PCI_MSGSTSMSK, 0x1, 2, ## args) \
	X(IDT_NT_OUTMSG3STSMSK, IDT_NT_PCI_MSGSTSMSK, 0x1, 3, ## args) \
	X(IDT_NT_INMSG0STSMSK,  IDT_NT_PCI_MSGSTSMSK, 0x1, 16, ## args) \
	X(IDT_NT_INMSG1STSMSK,  IDT_NT_PCI_MSGSTSMSK, 0x1, 17, ## args) \
	X(IDT_NT_INMSG2STSMSK,  IDT_NT_PCI_MSGSTSMSK, 0x1, 18, ## args) \
	X(IDT_NT_INMSG3STSMSK,  IDT_NT_PCI_MSGSTSMSK, 0x1, 19, ## args) \
	/* BARSETUPx register (default BARSETUP0) */ \
	X(IDT_NT_BARSTP_MEMSI,  IDT_NT_PCI_BARSETUP0, 0x1, 0, ## args) \
	X(IDT_NT_BARSTP_TYPE,   IDT_NT_PCI_BARSETUP0, 0x3, 1, ## args) \
	X(IDT_NT_BARSTP_PREF,   IDT_NT_PCI_BARSETUP0, 0x1, 3, ## args) \
	X(IDT_NT_BARSTP_SIZE,   IDT_NT_PCI_BARSETUP0, 0x3F, 4, ## args) \
	X(IDT_NT_BARSTP_MODE,   IDT_NT_PCI_BARSETUP0, 0x1, 10, ## args) \
	X(IDT_NT_BARSTP_ATRAN,  IDT_NT_PCI_BARSETUP0, 0x3, 11, ## args) \
	X(IDT_NT_BARSTP_TPART,  IDT_NT_PCI_BARSETUP0, 0x7, 13, ## args) \
	X(IDT_NT_BARSTP_EN,     IDT_NT_PCI_BARSETUP0, 0x1, 31, ## args) \
	/* NT mapping table registers */ \
	X(IDT_NT_MTBL_ADDR,     IDT_NT_PCI_NTMTBLADDR, 0x7F, 0, ## args) \
	X(IDT_NT_MTBL_ERR,      IDT_NT_PCI_NTMTBLSTS, 0x1, 0, ## args) \
	X(IDT_NT_MTBL_VALID,    IDT_NT_PCI_NTMTBLDATA, 0x1, 0, ## args) \
	X(IDT_NT_MTBL_BDF,      IDT_NT_PCI_NTMTBLDATA, 0xFFFF, 1, ## args) \
	X(IDT_NT_MTBL_PART,     IDT_NT_PCI_NTMTBLDATA, 0x7, 17, ## args) \
	X(IDT_NT_MTBL_ATP,      IDT_NT_PCI_NTMTBLDATA, 0x1, 29, ## args) \
	X(IDT_NT_MTBL_CNS,      IDT_NT_PCI_NTMTBLDATA, 0x1, 30, ## args) \
	X(IDT_NT_MTBL_RNS,      IDT_NT_PCI_NTMTBLDATA, 0x1, 31, ## args) \
	X(IDT_NT_MTBL_REQID,    IDT_NT_PCI_REQIDCAP, 0xFFFF, 0, ## args) \
	/* Lookup table registers */ \
	X(IDT_NT_LUT_INDEX,     IDT_NT_PCI_LUTOFFSET, 0x1F, 0, ## args) \
	X(IDT_NT_LUT_BAR,       IDT_NT_PCI_LUTOFFSET, 0x7, 8, ## args) \
	X(IDT_NT_LUT_PART,      IDT_NT_PCI_LUTUDATA, 0xF, 0, ## args) \
	X(IDT_NT_LUT_VALID,     IDT_NT_PCI_LUTUDATA, 0x1, 31, ## args)

/*
 * Table of the fields accessed over the global switch registers
 * This table is then translated into the switch-case statement
 * so to get the proper "Name"->{reg addr, mask, fld offset}
 * pairs.
 */
#define IDT_SW_REGFLDS(X, args...) \
	/* Boot configuration vector status */ \
	X(IDT_SW_SWMODE,          IDT_SW_PCI_BCVSTS, 0xF, 0, ## args) \
	X(IDT_SW_GCLKFSEL,        IDT_SW_PCI_BCVSTS, 0x1, 5, ## args) \
	X(IDT_SW_SSMB_ADDRSET,    IDT_SW_PCI_BCVSTS, 0x3, 7, ## args) \
	X(IDT_SW_CLKMODE,         IDT_SW_PCI_BCVSTS, 0x3, 14, ## args) \
	/* Ports clocking mode */ \
	X(IDT_SW_P0CLKMODE,       IDT_SW_PCI_PCLKMODE, 0x2, 0, ## args) \
	X(IDT_SW_P2CLKMODE,       IDT_SW_PCI_PCLKMODE, 0x2, 2, ## args) \
	X(IDT_SW_P4CLKMODE,       IDT_SW_PCI_PCLKMODE, 0x2, 4, ## args) \
	X(IDT_SW_P6CLKMODE,       IDT_SW_PCI_PCLKMODE, 0x2, 6, ## args) \
	X(IDT_SW_P8CLKMODE,       IDT_SW_PCI_PCLKMODE, 0x2, 8, ## args) \
	X(IDT_SW_P12CLKMODE,      IDT_SW_PCI_PCLKMODE, 0x2, 10, ## args) \
	X(IDT_SW_P16CLKMODE,      IDT_SW_PCI_PCLKMODE, 0x2, 12, ## args) \
	X(IDT_SW_P20CLKMODE,      IDT_SW_PCI_PCLKMODE, 0x2, 14, ## args) \
	/* Switch Ports Status register (default, Port 0) */ \
	X(IDT_SW_PORT_LNKUP,      IDT_SW_PCI_SWPORT0STS, 0x1, 4, ## args) \
	X(IDT_SW_PORT_LNKMODE,    IDT_SW_PCI_SWPORT0STS, 0x1, 5, ## args) \
	X(IDT_SW_PORT_MODE,       IDT_SW_PCI_SWPORT0STS, 0xF, 6, ## args) \
	X(IDT_SW_PORT_SWPART,     IDT_SW_PCI_SWPORT0STS, 0x7, 10, ## args) \
	/* Switch Event registers */ \
	X(IDT_SW_LNKUP_GSTS,      IDT_SW_PCI_SESTS, 0x1, 0, ## args) \
	X(IDT_SW_LNKDN_GSTS,      IDT_SW_PCI_SESTS, 0x1, 1, ## args) \
	X(IDT_SW_FRST_GSTS,       IDT_SW_PCI_SESTS, 0x1, 2, ## args) \
	X(IDT_SW_HRST_GSTS,       IDT_SW_PCI_SESTS, 0x1, 3, ## args) \
	X(IDT_SW_FOVER_GSTS,      IDT_SW_PCI_SESTS, 0x1, 4, ## args) \
	X(IDT_SW_GSIG_GSTS,       IDT_SW_PCI_SESTS, 0x1, 5, ## args) \
	X(IDT_SW_LNKUP_GMSK,      IDT_SW_PCI_SEMSK, 0x1, 0, ## args) \
	X(IDT_SW_LNKDN_GMSK,      IDT_SW_PCI_SEMSK, 0x1, 1, ## args) \
	X(IDT_SW_FRST_GMSK,       IDT_SW_PCI_SEMSK, 0x1, 2, ## args) \
	X(IDT_SW_HRST_GMSK,       IDT_SW_PCI_SEMSK, 0x1, 3, ## args) \
	X(IDT_SW_FOVER_GMSK,      IDT_SW_PCI_SEMSK, 0x1, 4, ## args) \
	X(IDT_SW_GSIG_GMSK,       IDT_SW_PCI_SEMSK, 0x1, 5, ## args) \
	X(IDT_SW_SEPART_GMSK,     IDT_SW_PCI_SEPMSK, 0xFF, 0, ## args) \
	X(IDT_SW_PORTLNKUP_STS,   IDT_SW_PCI_SELINKUPSTS, 0xFFF, 0, ## args) \
	X(IDT_SW_PORTLNKUP_MSK,   IDT_SW_PCI_SELINKUPMSK, 0xFFF, 0, ## args) \
	X(IDT_SW_PORTLNKDN_STS,   IDT_SW_PCI_SELINKDNSTS, 0xFFF, 0, ## args) \
	X(IDT_SW_PORTLNKDN_MSK,   IDT_SW_PCI_SELINKDNMSK, 0xFFF, 0, ## args) \
	X(IDT_SW_PARTFRST_STS,    IDT_SW_PCI_SEFRSTSTS, 0xF, 0, ## args) \
	X(IDT_SW_PARTFRST_MSK,    IDT_SW_PCI_SEFRSTMSK, 0xF, 0, ## args) \
	X(IDT_SW_PARTHRST_STS,    IDT_SW_PCI_SEHRSTSTS, 0xF, 0, ## args) \
	X(IDT_SW_PARTHRST_MSK,    IDT_SW_PCI_SEHRSTMSK, 0xF, 0, ## args) \
	X(IDT_SW_PARTGSIG_STS,    IDT_SW_PCI_SEGSIGSTS, 0xF, 0, ## args) \
	X(IDT_SW_PARTGSIG_MSK,    IDT_SW_PCI_SEGSIGMSK, 0xF, 0, ## args) \
	/* Global DoorBell registers (default, Doorbell 0) */ \
	X(IDT_SW_PART_GODBELLMSK, IDT_SW_PCI_GODBELLMSK0, 0xF, 0, ## args) \
	X(IDT_SW_PART_GIDBELLMSK, IDT_SW_PCI_GIDBELLMSK0, 0xF, 0, ## args) \
	/* Message register (default, Partition 0 Message Control 0) */ \
	X(IDT_SW_MSGROUTE_REG,    IDT_SW_PCI_SWP0MSGCTL0, 0x3, 0, ## args) \
	X(IDT_SW_MSGROUTE_PART,   IDT_SW_PCI_SWP0MSGCTL0, 0x7, 4, ## args) \
	/* SMBUS status */ \
	X(IDT_SW_SSMBADDR,        IDT_SW_PCI_SMBUSSTS, 0x7F, 1, ## args) \
	X(IDT_SW_MSMBADDR,        IDT_SW_PCI_SMBUSSTS, 0x7F, 9, ## args) \
	/* Temperature sensor register */ \
	X(IDT_SW_TMP_LTH,         IDT_SW_PCI_TMPCTL, 0xFF, 0, ## args) \
	X(IDT_SW_TMP_HTH,         IDT_SW_PCI_TMPCTL, 0xFF, 16, ## args) \
	X(IDT_SW_TMP_BLTH_EN,     IDT_SW_PCI_TMPCTL, 0x1, 26, ## args) \
	X(IDT_SW_TMP_AHTH_EN,     IDT_SW_PCI_TMPCTL, 0x1, 29, ## args) \
	X(IDT_SW_TMP_PDOWN,       IDT_SW_PCI_TMPCTL, 0x1, 31, ## args) \
	X(IDT_SW_TMP_CURTEMP,     IDT_SW_PCI_TMPSTS, 0xFF, 0, ## args) \
	X(IDT_SW_TMP_BLTH_STS,    IDT_SW_PCI_TMPSTS, 0x1, 24, ## args) \
	X(IDT_SW_TMP_AHTH_STS,    IDT_SW_PCI_TMPSTS, 0x1, 29, ## args) \
	X(IDT_SW_TMP_BLTH_CLR,    IDT_SW_PCI_TMPALARM, 0x1, 24, ## args) \
	X(IDT_SW_TMP_AHTH_CLR,    IDT_SW_PCI_TMPALARM, 0x1, 29, ## args)

/*
 * Enumeration of the IDT PCIe-switch registers access fields
 */
enum idt_ntb_regfld {
	IDT_NT_REGFLDS(PAIR_ID_ENUM)
	IDT_NTB_REGFLDS_SPLIT,
	IDT_SW_REGFLDS(PAIR_ID_ENUM)
	IDT_NTB_REGFLDS_END
};

/*
 * Enumeration of the possible registers size
 */
enum idt_ntb_regsize {
	REGBYTE = 1,
	REGWORD = 2,
	REGDWORD = 4
};

/*
 * Enumeration of the NT-function Configuration Space registers
 * NOTE 1) The IDT PCIe-switch internal data is littel-endian
 *      so it must be taken into account in the driver
 *      internals.
 *      2) Additionally the registers should be accessed either
 *      with byte-enables corresponding to their native size or
 *      the size of one DWORD
 */
#define IDT_NT_CFGREGS(X, args...) \
	/* PCI Express Configuration Space */ \
	/* Type 0 configuration header */ \
	X(IDT_NT_PCI_VID,          0x00000, REGWORD, "Vendor Identification", ## args) \
	X(IDT_NT_PCI_DID,          0x00002, REGWORD, "Device Identification", ## args) \
	X(IDT_NT_PCI_CMD,          0x00004, REGWORD, "PCI Command", ## args) \
	X(IDT_NT_PCI_STS,          0x00006, REGWORD, "PCI Status", ## args) \
	X(IDT_NT_PCI_RID,          0x00008, REGBYTE, "Revision Identification", ## args) \
	X(IDT_NT_PCI_PROGIF,       0x00009, REGBYTE, "Program Interface", ## args) \
	X(IDT_NT_PCI_SCCLASS,      0x0000A, REGBYTE, "Sub Class Code", ## args) \
	X(IDT_NT_PCI_CLASS,        0x0000B, REGBYTE, "Class Code", ## args) \
	X(IDT_NT_PCI_CLS,          0x0000C, REGBYTE, "Cache Line Size", ## args) \
	X(IDT_NT_PCI_LTIMER,       0x0000D, REGBYTE, "Latency Time", ## args) \
	X(IDT_NT_PCI_HDR,          0x0000E, REGBYTE, "Header Type", ## args) \
	X(IDT_NT_PCI_BIST,         0x0000F, REGBYTE, "Built-in Self Test Register", ## args) \
	X(IDT_NT_PCI_BAR0,         0x00010, REGDWORD, "Base Address Register 0", ## args) \
	X(IDT_NT_PCI_BAR1,         0x00014, REGDWORD, "Base Address Register 1", ## args) \
	X(IDT_NT_PCI_BAR2,         0x00018, REGDWORD, "Base Address Register 2", ## args) \
	X(IDT_NT_PCI_BAR3,         0x0001C, REGDWORD, "Base Address Register 3", ## args) \
	X(IDT_NT_PCI_BAR4,         0x00020, REGDWORD, "Base Address Register 4", ## args) \
	X(IDT_NT_PCI_BAR5,         0x00024, REGDWORD, "Base Address Register 5", ## args) \
	X(IDT_NT_PCI_CCISPTR,      0x00028, REGDWORD, "CardBus CIS Pointer", ## args) \
	X(IDT_NT_PCI_SUBVID,       0x0002C, REGWORD, "Subsystem Vendor ID Pointer", ## args) \
	X(IDT_NT_PCI_SUBID,        0x0002E, REGWORD, "Subsystem ID Pointer", ## args) \
	X(IDT_NT_PCI_EROMBASE,     0x00030, REGWORD, "Expansion ROM Base", ## args) \
	X(IDT_NT_PCI_CAPPTR,       0x00034, REGBYTE, "Capabilities Pointer", ## args) \
	X(IDT_NT_PCI_INTRLINE,     0x0003C, REGBYTE, "Interrupt Line", ## args) \
	X(IDT_NT_PCI_INTRPIN,      0x0003D, REGBYTE, "Interrupt PIN", ## args) \
	X(IDT_NT_PCI_MINGNT,       0x0003E, REGBYTE, "Minimum Grant", ## args) \
	X(IDT_NT_PCI_MAXLAT,       0x0003F, REGBYTE, "Maximum Latency", ## args) \
	/* PCI Express capablity structure */ \
	X(IDT_NT_PCI_PCIECAP,      0x00040, REGDWORD, "PCI Express Capability", ## args) \
	X(IDT_NT_PCI_PCIEDCAP,     0x00044, REGDWORD, "PCI Express Device Capabilities", ## args) \
	X(IDT_NT_PCI_PCIEDCTL,     0x00048, REGWORD, "PCI Express Device Control", ## args) \
	X(IDT_NT_PCI_PCIEDSTS,     0x0004A, REGWORD, "PCI Express Device Status", ## args) \
	X(IDT_NT_PCI_PCIELCAP,     0x0004C, REGDWORD, "PCI Express Link Capabilities", ## args) \
	X(IDT_NT_PCI_PCIELCTL,     0x00050, REGWORD, "PCI Express Link Control", ## args) \
	X(IDT_NT_PCI_PCIELSTS,     0x00052, REGWORD, "PCI Express Link Status", ## args) \
	X(IDT_NT_PCI_PCIEDCAP2,    0x00064, REGDWORD, "PCI Express Device Capabilities 2", ## args) \
	X(IDT_NT_PCI_PCIEDCTL2,    0x00068, REGWORD, "PCI Express Device Control 2", ## args) \
	X(IDT_NT_PCI_PCIEDSTS2,    0x0006A, REGWORD, "PCI Express Device Status 2", ## args) \
	X(IDT_NT_PCI_PCIELCAP2,    0x0006C, REGDWORD, "PCI Express Link Capabilities 2", ## args) \
	X(IDT_NT_PCI_PCIELCTL2,    0x00070, REGWORD, "PCI Express Link Control 2", ## args) \
	X(IDT_NT_PCI_PCIELSTS2,    0x00072, REGWORD, "PCI Express Link Status 2", ## args) \
	/* PCI Power Management capability structure */ \
	X(IDT_NT_PCI_PMCAP,        0x000C0, REGDWORD, "PCI Power Management Capabilities", ## args) \
	X(IDT_NT_PCI_PMCSR,        0x000C4, REGDWORD, "PCI Power Management Control and Status", ## args) \
	/* MSI Capability structure */ \
	X(IDT_NT_PCI_MSICAP,       0x000D0, REGDWORD, "Message Signaled Interrupt Capability and Control", ## args) \
	X(IDT_NT_PCI_MSIADDR,      0x000D4, REGDWORD, "Message Signaled Interrupt Address", ## args) \
	X(IDT_NT_PCI_MSIUADDR,     0x000D8, REGDWORD, "Message Signaled Interrupt Upper Address", ## args) \
	X(IDT_NT_PCI_MSIMDATA,     0x000DC, REGDWORD, "Message Signaled Interrupt Message Data", ## args) \
	/* SSID/SSVID capability structure */ \
	X(IDT_NT_PCI_SSIDSSVIDCAP, 0x000F0, REGDWORD, "Subsystem ID and Subsystem Vendor ID Capability", ## args) \
	X(IDT_NT_PCI_SSIDSSVID,    0x000F4, REGDWORD, "Subsystem ID and Subsystem Vendor ID", ## args) \
	/* Extended access registers */ \
	X(IDT_NT_PCI_ECFGADDR,     0x000F8, REGDWORD, "Extended Configuration Space Access Address", ## args) \
	X(IDT_NT_PCI_ECFGDATA,     0x000FC, REGDWORD, "Extended Configuration Space Access Data", ## args) \
	/*==============64 REGDWORDs ================*/ \
	/* PCI Express Extended Configuration Space */ \
	/* Advanced Error Reporting enhanced capability */ \
	X(IDT_NT_PCI_AERCAP,       0x00100, REGDWORD, "AER Capabilities ", ## args) \
	X(IDT_NT_PCI_AERUES,       0x00104, REGDWORD, "AER Uncorrectable Error Status", ## args) \
	X(IDT_NT_PCI_AERUEM,       0x00108, REGDWORD, "AER Uncorrectable Error Mask ", ## args) \
	X(IDT_NT_PCI_AERUESV,      0x0010C, REGDWORD, "AER Uncorrectable Error Severity ", ## args) \
	X(IDT_NT_PCI_AERCES,       0x00110, REGDWORD, "AER Correctable Error Status ", ## args) \
	X(IDT_NT_PCI_AERCEM,       0x00114, REGDWORD, "AER Correctable Error Mask", ## args) \
	X(IDT_NT_PCI_AERCTL,       0x00118, REGDWORD, "AER Control", ## args) \
	X(IDT_NT_PCI_AERHL1DW,     0x0011C, REGDWORD, "AER Header Log 1st Doubleword", ## args) \
	X(IDT_NT_PCI_AERHL2DW,     0x00120, REGDWORD, "AER Header Log 2nd Doubleword", ## args) \
	X(IDT_NT_PCI_AERHL3DW,     0x00124, REGDWORD, "AER Header Log 3rd Doubleword", ## args) \
	X(IDT_NT_PCI_AERHL4DW,     0x00128, REGDWORD, "AER Header Log 4th Doubleword", ## args) \
	/* Device Serial Number enhanced capability */ \
	X(IDT_NT_PCI_SNUMCAP,      0x00180, REGDWORD, "Serial Number Capabilities", ## args) \
	X(IDT_NT_PCI_SNUMLDW,      0x00184, REGDWORD, "Serial Number Lower Doubleword", ## args) \
	X(IDT_NT_PCI_SNUMUDW,      0x00188, REGDWORD, "Serial Number Upper Doubleword", ## args) \
	/* PCIe Virtual Channel enhanced capability */ \
	X(IDT_NT_PCI_PCIEVCECAP,   0x00200, REGDWORD, "PCI Express VC Extended Capability Header", ## args) \
	X(IDT_NT_PCI_PVCCAP1,      0x00204, REGDWORD, "Port VC Capability 1", ## args) \
	X(IDT_NT_PCI_PVCCAP2,      0x00208, REGDWORD, "Port VC Capability 2", ## args) \
	X(IDT_NT_PCI_PVCCTL,       0x0020C, REGDWORD, "Port VC Control", ## args) \
	X(IDT_NT_PCI_PVCSTS,       0x0020E, REGDWORD, "Port VC Status ", ## args) \
	X(IDT_NT_PCI_VCR0CAP,      0x00210, REGDWORD, "VC Resource 0 Capability", ## args) \
	X(IDT_NT_PCI_VCR0CTL,      0x00214, REGDWORD, "VC Resource 0 Control", ## args) \
	X(IDT_NT_PCI_VCR0STS,      0x00218, REGDWORD, "VC Resource 0 Status", ## args) \
	/* ACS enhanced capability */ \
	X(IDT_NT_PCI_ACSECAPH,     0x00320, REGDWORD, "ACS Extended Capability Header", ## args) \
	X(IDT_NT_PCI_ACSCAP,       0x00324, REGWORD, "ACS Capability", ## args) \
	X(IDT_NT_PCI_ACSCTL,       0x00326, REGWORD, "ACS Control", ## args) \
	X(IDT_NT_PCI_MCCAPH,       0x00330, REGDWORD, "Multicast Extended Capability Header", ## args) \
	X(IDT_NT_PCI_MCCAP,        0x00334, REGWORD, "Multicast Capability", ## args) \
	X(IDT_NT_PCI_MCCTL,        0x00336, REGWORD, "Multicast Control", ## args) \
	X(IDT_NT_PCI_MCBARL,       0x00338, REGDWORD, "Multicast Base Address Low", ## args) \
	X(IDT_NT_PCI_MCBARH,       0x0033C, REGDWORD, "Multicast Base Address High", ## args) \
	X(IDT_NT_PCI_MCRCVL,       0x00340, REGDWORD, "Multicast Receive Low", ## args) \
	X(IDT_NT_PCI_MCRCVH,       0x00344, REGDWORD, "Multicast Receive High", ## args) \
	X(IDT_NT_PCI_MCBLKALLL,    0x00348, REGDWORD, "Multicast Block All Low", ## args) \
	X(IDT_NT_PCI_MCBLKALLH,    0x0034C, REGDWORD, "Multicast Block All High", ## args) \
	X(IDT_NT_PCI_MCBLKUTL,     0x00350, REGDWORD, "Multicast Block Untranslated Low", ## args) \
	X(IDT_NT_PCI_MCBLKUTH,     0x00354, REGDWORD, "Multicast Block Untranslated High", ## args) \
	/*==========================================*/ \
	/* IDT Proprietary NT-port-specific registers */ \
	/* NT-function main control registers */ \
	X(IDT_NT_PCI_NTCTL,        0x00400, REGDWORD, "NT Endpoint Control", ## args) \
	X(IDT_NT_PCI_NTINTSTS,     0x00404, REGDWORD, "NT Endpoint Interrupt Status", ## args) \
	X(IDT_NT_PCI_NTINTMSK,     0x00408, REGDWORD, "NT Endpoint Interrupt Mask", ## args) \
	X(IDT_NT_PCI_NTSDATA,      0x0040C, REGDWORD, "NT Endpoint Signal Data", ## args) \
	X(IDT_NT_PCI_NTGSIGNAL,    0x00410, REGDWORD, "NT Endpoint Global Signal", ## args) \
	X(IDT_NT_PCI_NTIERRORMSK0, 0x00414, REGDWORD, "Internal Error Reporting Mask 0", ## args) \
	X(IDT_NT_PCI_NTIERRORMSK1, 0x00418, REGDWORD, "Internal Error Reporting Mask 1", ## args) \
	/* Doorbel registers */ \
	X(IDT_NT_PCI_OUTDBELLSET,  0x00420, REGDWORD, "NT Outbound Doorbell Set", ## args) \
	X(IDT_NT_PCI_INDBELLSTS,   0x00428, REGDWORD, "NT Inbound Doorbell Status", ## args) \
	X(IDT_NT_PCI_INDBELLMSK,   0x0042C, REGDWORD, "NT Inbound Doorbell Mask", ## args) \
	/* Message registers */ \
	X(IDT_NT_PCI_OUTMSG0,      0x00430, REGDWORD, "Outbound Message 0", ## args) \
	X(IDT_NT_PCI_OUTMSG1,      0x00434, REGDWORD, "Outbound Message 1", ## args) \
	X(IDT_NT_PCI_OUTMSG2,      0x00438, REGDWORD, "Outbound Message 2", ## args) \
	X(IDT_NT_PCI_OUTMSG3,      0x0043C, REGDWORD, "Outbound Message 3", ## args) \
	X(IDT_NT_PCI_INMSG0,       0x00440, REGDWORD, "Inbound Message 0", ## args) \
	X(IDT_NT_PCI_INMSG1,       0x00444, REGDWORD, "Inbound Message 1", ## args) \
	X(IDT_NT_PCI_INMSG2,       0x00448, REGDWORD, "Inbound Message 2", ## args) \
	X(IDT_NT_PCI_INMSG3,       0x0044C, REGDWORD, "Inbound Message 3", ## args) \
	X(IDT_NT_PCI_INMSGSRC0,    0x00450, REGDWORD, "Inbound Message Source 0", ## args) \
	X(IDT_NT_PCI_INMSGSRC1,    0x00454, REGDWORD, "Inbound Message Source 1", ## args) \
	X(IDT_NT_PCI_INMSGSRC2,    0x00458, REGDWORD, "Inbound Message Source 2", ## args) \
	X(IDT_NT_PCI_INMSGSRC3,    0x0045C, REGDWORD, "Inbound Message Source 3", ## args) \
	X(IDT_NT_PCI_MSGSTS,       0x00460, REGDWORD, "Message Status", ## args) \
	X(IDT_NT_PCI_MSGSTSMSK,    0x00464, REGDWORD, "Message Status Mask", ## args) \
	/* BAR-setup registers */ \
	X(IDT_NT_PCI_BARSETUP0,    0x00470, REGDWORD, "BAR 0 Setup", ## args) \
	X(IDT_NT_PCI_BARLIMIT0,    0x00474, REGDWORD, "BAR 0 Limit Address", ## args) \
	X(IDT_NT_PCI_BARLTBASE0,   0x00478, REGDWORD, "BAR 0 Lower Translated Base Address", ## args) \
	X(IDT_NT_PCI_BARUTBASE0,   0x0047C, REGDWORD, "BAR 0 Upper Translated Base Address", ## args) \
	X(IDT_NT_PCI_BARSETUP1,    0x00480, REGDWORD, "BAR 1 Setup", ## args) \
	X(IDT_NT_PCI_BARLIMIT1,    0x00484, REGDWORD, "BAR 1 Limit Address", ## args) \
	X(IDT_NT_PCI_BARLTBASE1,   0x00488, REGDWORD, "BAR 1 Lower Translated Base Address", ## args) \
	X(IDT_NT_PCI_BARUTBASE1,   0x0048C, REGDWORD, "BAR 1 Upper Translated Base Address", ## args) \
	X(IDT_NT_PCI_BARSETUP2,    0x00490, REGDWORD, "BAR 2 Setup", ## args) \
	X(IDT_NT_PCI_BARLIMIT2,    0x00494, REGDWORD, "BAR 2 Limit Address", ## args) \
	X(IDT_NT_PCI_BARLTBASE2,   0x00498, REGDWORD, "BAR 2 Lower Translated Base Address", ## args) \
	X(IDT_NT_PCI_BARUTBASE2,   0x0049C, REGDWORD, "BAR 2 Upper Translated Base Address", ## args) \
	X(IDT_NT_PCI_BARSETUP3,    0x004A0, REGDWORD, "BAR 3 Setup", ## args) \
	X(IDT_NT_PCI_BARLIMIT3,    0x004A4, REGDWORD, "BAR 3 Limit Address", ## args) \
	X(IDT_NT_PCI_BARLTBASE3,   0x004A8, REGDWORD, "BAR 3 Lower Translated Base Address", ## args) \
	X(IDT_NT_PCI_BARUTBASE3,   0x004AC, REGDWORD, "BAR 3 Upper Translated Base Address", ## args) \
	X(IDT_NT_PCI_BARSETUP4,    0x004B0, REGDWORD, "BAR 4 Setup", ## args) \
	X(IDT_NT_PCI_BARLIMIT4,    0x004B4, REGDWORD, "BAR 4 Limit Address", ## args) \
	X(IDT_NT_PCI_BARLTBASE4,   0x004B8, REGDWORD, "BAR 4 Lower Translated Base Address", ## args) \
	X(IDT_NT_PCI_BARUTBASE4,   0x004BC, REGDWORD, "BAR 4 Upper Translated Base Address", ## args) \
	X(IDT_NT_PCI_BARSETUP5,    0x004C0, REGDWORD, "BAR 5 Setup", ## args) \
	X(IDT_NT_PCI_BARLIMIT5,    0x004C4, REGDWORD, "BAR 5 Limit Address", ## args) \
	X(IDT_NT_PCI_BARLTBASE5,   0x004C8, REGDWORD, "BAR 5 Lower Translated Base Address", ## args) \
	X(IDT_NT_PCI_BARUTBASE5,   0x004CC, REGDWORD, "BAR 5 Upper Translated Base Address", ## args) \
	/* NT mapping table registers */ \
	X(IDT_NT_PCI_NTMTBLADDR,   0x004D0, REGDWORD, "NT Mapping Table Address", ## args) \
	X(IDT_NT_PCI_NTMTBLSTS,    0x004D4, REGDWORD, "NT Mapping Table Status", ## args) \
	X(IDT_NT_PCI_NTMTBLDATA,   0x004D8, REGDWORD, "NT Mapping Table Data", ## args) \
	X(IDT_NT_PCI_REQIDCAP,     0x004DC, REGDWORD, "Requester ID Capture", ## args) \
	/* Memory Windows Lookup table registers */ \
	X(IDT_NT_PCI_LUTOFFSET,    0x004E0, REGDWORD, "Lookup Table Offset", ## args) \
	X(IDT_NT_PCI_LUTLDATA,     0x004E4, REGDWORD, "Lookup Table Lower Data", ## args) \
	X(IDT_NT_PCI_LUTMDATA,     0x004E8, REGDWORD, "Lookup Table Middle Data", ## args) \
	X(IDT_NT_PCI_LUTUDATA,     0x004EC, REGDWORD, "Lookup Table Upper Data", ## args) \
	/* NT Endpoint Errors Emulation registers */ \
	X(IDT_NT_PCI_NTUEEM,       0x004F0, REGDWORD, "NT Endpoint Uncorrectable Error Emulation", ## args) \
	X(IDT_NT_PCI_NTCEEM,       0x004F4, REGDWORD, "NT Endpoint Correctable Error Emulation", ## args) \
	/* Punch-through registers */ \
	X(IDT_NT_PCI_PTCCTL0,      0x00510, REGDWORD, "Punch-Through Configuration Control 0", ## args) \
	X(IDT_NT_PCI_PTCCTL1,      0x00514, REGDWORD, "Punch-Through Configuration Control 1", ## args) \
	X(IDT_NT_PCI_PTCDATA,      0x00518, REGDWORD, "Punch-Through Data", ## args) \
	X(IDT_NT_PCI_PTCSTS,       0x0051C, REGDWORD, "Punch-Through Status", ## args) \
	/* NT Multicast Group x Port association */ \
	X(IDT_NT_PCI_NTMCG0PA,     0x00600, REGDWORD, "NT Multicast Group x Port Association", ## args) \
	X(IDT_NT_PCI_NTMCG1PA,     0x00604, REGDWORD, "NT Multicast Group x Port Association", ## args) \
	X(IDT_NT_PCI_NTMCG2PA,     0x00608, REGDWORD, "NT Multicast Group x Port Association", ## args) \
	X(IDT_NT_PCI_NTMCG3PA,     0x0060C, REGDWORD, "NT Multicast Group x Port Association", ## args) \
	/* Global Address Space Access registers */ \
	/*X(IDT_NT_PCI_GASAADDR,     0x00FF8, REGDWORD, "Global Address Space Access Address", ## args) \
	 *X(IDT_NT_PCI_GASADATA,     0x00FFC, REGDWORD, "Global Address Space Access Data", ## args)*/

/*
 * Table of the IDT PCIe-switch Global Configuration and Status
 * registers, corresponding size and the string name
 */
#define IDT_SW_CFGREGS(X, args...) \
	/* Basic NT-function globally accessed registers */ \
	/* Port 0 */ \
	X(IDT_SW_PCI_NTP0_CMD,         0x01004, REGWORD, "Port 0 PCI Command", ## args) \
	X(IDT_SW_PCI_NTP0_PCIELSTS,    0x01052, REGWORD, "Port 0 PCIe link status", ## args) \
	X(IDT_SW_PCI_NTP0_NTSDATA,     0x0140C, REGDWORD, "Port 0 NT Signal data", ## args) \
	X(IDT_SW_PCI_NTP0_NTGSIGNAL,   0x01410, REGDWORD, "Port 0 NT Global Signal", ## args) \
	/* Port 2 */ \
	X(IDT_SW_PCI_NTP2_CMD,         0x05004, REGWORD, "Port 2 PCI Command", ## args) \
	X(IDT_SW_PCI_NTP2_PCIELSTS,    0x05052, REGWORD, "Port 2 PCIe link status", ## args) \
	X(IDT_SW_PCI_NTP2_NTSDATA,     0x0540C, REGDWORD, "Port 2 NT Signal data", ## args) \
	X(IDT_SW_PCI_NTP2_NTGSIGNAL,   0x05410, REGDWORD, "Port 2 NT Global Signal", ## args) \
	/* Port 4 */ \
	X(IDT_SW_PCI_NTP4_CMD,         0x09004, REGWORD, "Port 4 PCI Command", ## args) \
	X(IDT_SW_PCI_NTP4_PCIELSTS,    0x09052, REGWORD, "Port 4 PCIe link status", ## args) \
	X(IDT_SW_PCI_NTP4_NTSDATA,     0x0940C, REGDWORD, "Port 4 NT Signal data", ## args) \
	X(IDT_SW_PCI_NTP4_NTGSIGNAL,   0x09410, REGDWORD, "Port 4 NT Global Signal", ## args) \
	/* Port 6 */ \
	X(IDT_SW_PCI_NTP6_CMD,         0x0D004, REGWORD, "Port 6 PCI Command", ## args) \
	X(IDT_SW_PCI_NTP6_PCIELSTS,    0x0D052, REGWORD, "Port 6 PCIe link status", ## args) \
	X(IDT_SW_PCI_NTP6_NTSDATA,     0x0D40C, REGDWORD, "Port 6 NT Signal data", ## args) \
	X(IDT_SW_PCI_NTP6_NTGSIGNAL,   0x0D410, REGDWORD, "Port 6 NT Global Signal", ## args) \
	/* Port 8 */ \
	X(IDT_SW_PCI_NTP8_CMD,         0x11004, REGWORD, "Port 8 PCI Command", ## args) \
	X(IDT_SW_PCI_NTP8_PCIELSTS,    0x11052, REGWORD, "Port 8 PCIe link status", ## args) \
	X(IDT_SW_PCI_NTP8_NTSDATA,     0x1140C, REGDWORD, "Port 8 NT Signal data", ## args) \
	X(IDT_SW_PCI_NTP8_NTGSIGNAL,   0x11410, REGDWORD, "Port 8 NT Global Signal", ## args) \
	/* Port 12 */ \
	X(IDT_SW_PCI_NTP12_CMD,        0x19004, REGWORD, "Port 12 PCI Command", ## args) \
	X(IDT_SW_PCI_NTP12_PCIELSTS,   0x19052, REGWORD, "Port 12 PCIe link status", ## args) \
	X(IDT_SW_PCI_NTP12_NTSDATA,    0x1940C, REGDWORD, "Port 12 NT Signal data", ## args) \
	X(IDT_SW_PCI_NTP12_NTGSIGNAL,  0x19410, REGDWORD, "Port 12 NT Global Signal", ## args) \
	/* Port 16 */ \
	X(IDT_SW_PCI_NTP16_CMD,        0x21004, REGWORD, "Port 16 PCI Command", ## args) \
	X(IDT_SW_PCI_NTP16_PCIELSTS,   0x21052, REGWORD, "Port 16 PCIe link status", ## args) \
	X(IDT_SW_PCI_NTP16_NTSDATA,    0x2140C, REGDWORD, "Port 16 NT Signal data", ## args) \
	X(IDT_SW_PCI_NTP16_NTGSIGNAL,  0x21410, REGDWORD, "Port 16 NT Global Signal", ## args) \
	/* Port 20 */ \
	X(IDT_SW_PCI_NTP20_CMD,        0x29004, REGWORD, "Port 20 PCI Command", ## args) \
	X(IDT_SW_PCI_NTP20_PCIELSTS,   0x29052, REGWORD, "Port 20 PCIe link status", ## args) \
	X(IDT_SW_PCI_NTP20_NTSDATA,    0x2940C, REGDWORD, "Port 20 NT Signal data", ## args) \
	X(IDT_SW_PCI_NTP20_NTGSIGNAL,  0x29410, REGDWORD, "Port 20 NT Global Signal", ## args) \
	/* IDT PCIe-switch control registers */ \
	X(IDT_SW_PCI_SWCTL,        0x3E000, REGDWORD, "Switch Control", ## args) \
	X(IDT_SW_PCI_BCVSTS,       0x3E004, REGDWORD, "Boot Configuration Vector Status", ## args) \
	X(IDT_SW_PCI_PCLKMODE,     0x3E008, REGDWORD, "Port Clocking Mode", ## args) \
	X(IDT_SW_PCI_STK0CFG,      0x3E010, REGDWORD, "Stack 0 Configuration", ## args) \
	X(IDT_SW_PCI_STK1CFG,      0x3E014, REGDWORD, "Stack 1 Configuration", ## args) \
	X(IDT_SW_PCI_STK2CFG,      0x3E018, REGDWORD, "Stack 2 Configuration", ## args) \
	X(IDT_SW_PCI_STK3CFG,      0x3E01C, REGDWORD, "Stack 3 Configuration", ## args) \
	/* Switch initialization delays */ \
	X(IDT_SW_PCI_RDRAINDELAY,  0x3E080, REGDWORD, "Reset Drain Delay ", ## args) \
	X(IDT_SW_PCI_POMCDELAY,    0x3E084, REGDWORD, "Port Operating Mode Change Drain Delay", ## args) \
	X(IDT_SW_PCI_SEDELAY,      0x3E088, REGDWORD, "Side Effect Delay", ## args) \
	X(IDT_SW_PCI_USSBRDELAY,   0x3E08C, REGDWORD, "Upstream Secondary Bus Reset Delay", ## args) \
	/* Switch Partitions control and status registers */ \
	X(IDT_SW_PCI_SWPART0CTL,   0x3E100, REGDWORD, "Switch Partition 0 Control", ## args) \
	X(IDT_SW_PCI_SWPART0STS,   0x3E104, REGDWORD, "Switch Partition 0 Status", ## args) \
	X(IDT_SW_PCI_SWPART0FCTL,  0x3E108, REGDWORD, "Switch Partition 0 Failover Control", ## args) \
	X(IDT_SW_PCI_SWPART1CTL,   0x3E120, REGDWORD, "Switch Partition 1 Control", ## args) \
	X(IDT_SW_PCI_SWPART1STS,   0x3E124, REGDWORD, "Switch Partition 1 Status", ## args) \
	X(IDT_SW_PCI_SWPART1FCTL,  0x3E128, REGDWORD, "Switch Partition 1 Failover Control", ## args) \
	X(IDT_SW_PCI_SWPART2CTL,   0x3E140, REGDWORD, "Switch Partition 2 Control", ## args) \
	X(IDT_SW_PCI_SWPART2STS,   0x3E144, REGDWORD, "Switch Partition 2 Status", ## args) \
	X(IDT_SW_PCI_SWPART2FCTL,  0x3E148, REGDWORD, "Switch Partition 2 Failover Control", ## args) \
	X(IDT_SW_PCI_SWPART3CTL,   0x3E160, REGDWORD, "Switch Partition 3 Control", ## args) \
	X(IDT_SW_PCI_SWPART3STS,   0x3E164, REGDWORD, "Switch Partition 3 Status", ## args) \
	X(IDT_SW_PCI_SWPART3FCTL,  0x3E168, REGDWORD, "Switch Partition 3 Failover Control", ## args) \
	X(IDT_SW_PCI_SWPART4CTL,   0x3E180, REGDWORD, "Switch Partition 4 Control", ## args) \
	X(IDT_SW_PCI_SWPART4STS,   0x3E184, REGDWORD, "Switch Partition 4 Status", ## args) \
	X(IDT_SW_PCI_SWPART4FCTL,  0x3E188, REGDWORD, "Switch Partition 4 Failover Control", ## args) \
	X(IDT_SW_PCI_SWPART5CTL,   0x3E1A0, REGDWORD, "Switch Partition 5 Control", ## args) \
	X(IDT_SW_PCI_SWPART5STS,   0x3E1A4, REGDWORD, "Switch Partition 5 Status", ## args) \
	X(IDT_SW_PCI_SWPART5FCTL,  0x3E1A8, REGDWORD, "Switch Partition 5 Failover Control", ## args) \
	X(IDT_SW_PCI_SWPART6CTL,   0x3E1C0, REGDWORD, "Switch Partition 6 Control", ## args) \
	X(IDT_SW_PCI_SWPART6STS,   0x3E1C4, REGDWORD, "Switch Partition 6 Status", ## args) \
	X(IDT_SW_PCI_SWPART6FCTL,  0x3E1C8, REGDWORD, "Switch Partition 6 Failover Control", ## args) \
	X(IDT_SW_PCI_SWPART7CTL,   0x3E1E0, REGDWORD, "Switch Partition 7 Control", ## args) \
	X(IDT_SW_PCI_SWPART7STS,   0x3E1E4, REGDWORD, "Switch Partition 7 Status", ## args) \
	X(IDT_SW_PCI_SWPART7FCTL,  0x3E1E8, REGDWORD, "Switch Partition 7 Failover Control", ## args) \
	/* Switch Ports control and status registers */ \
	X(IDT_SW_PCI_SWPORT0CTL,   0x3E200, REGDWORD, "Switch Port 0 Control", ## args) \
	X(IDT_SW_PCI_SWPORT0STS,   0x3E204, REGDWORD, "Switch Port 0 Status", ## args) \
	X(IDT_SW_PCI_SWPORT0FCTL,  0x3E208, REGDWORD, "Switch Port 0 Failover Control", ## args) \
	X(IDT_SW_PCI_SWPORT2CTL,   0x3E240, REGDWORD, "Switch Port 2 Control", ## args) \
	X(IDT_SW_PCI_SWPORT2STS,   0x3E244, REGDWORD, "Switch Port 2 Status", ## args) \
	X(IDT_SW_PCI_SWPORT2FCTL,  0x3E248, REGDWORD, "Switch Port 2 Failover Control", ## args) \
	X(IDT_SW_PCI_SWPORT4CTL,   0x3E280, REGDWORD, "Switch Port 4 Control", ## args) \
	X(IDT_SW_PCI_SWPORT4STS,   0x3E284, REGDWORD, "Switch Port 4 Status", ## args) \
	X(IDT_SW_PCI_SWPORT4FCTL,  0x3E288, REGDWORD, "Switch Port 4 Failover Control", ## args) \
	X(IDT_SW_PCI_SWPORT6CTL,   0x3E2C0, REGDWORD, "Switch Port 6 Control", ## args) \
	X(IDT_SW_PCI_SWPORT6STS,   0x3E2C4, REGDWORD, "Switch Port 6 Status", ## args) \
	X(IDT_SW_PCI_SWPORT6FCTL,  0x3E2C8, REGDWORD, "Switch Port 6 Failover Control", ## args) \
	X(IDT_SW_PCI_SWPORT8CTL,   0x3E300, REGDWORD, "Switch Port 8 Control", ## args) \
	X(IDT_SW_PCI_SWPORT8STS,   0x3E304, REGDWORD, "Switch Port 8 Status", ## args) \
	X(IDT_SW_PCI_SWPORT8FCTL,  0x3E308, REGDWORD, "Switch Port 8 Failover Control", ## args) \
	X(IDT_SW_PCI_SWPORT12CTL,  0x3E380, REGDWORD, "Switch Port 12 Control", ## args) \
	X(IDT_SW_PCI_SWPORT12STS,  0x3E384, REGDWORD, "Switch Port 12 Status", ## args) \
	X(IDT_SW_PCI_SWPORT12FCTL, 0x3E388, REGDWORD, "Switch Port 12 Failover Control", ## args) \
	X(IDT_SW_PCI_SWPORT16CTL,  0x3E400, REGDWORD, "Switch Port 16 Control", ## args) \
	X(IDT_SW_PCI_SWPORT16STS,  0x3E404, REGDWORD, "Switch Port 16 Status", ## args) \
	X(IDT_SW_PCI_SWPORT16FCTL, 0x3E408, REGDWORD, "Switch Port 16 Failover Control", ## args) \
	X(IDT_SW_PCI_SWPORT20CTL,  0x3E480, REGDWORD, "Switch Port 20 Control", ## args) \
	X(IDT_SW_PCI_SWPORT20STS,  0x3E484, REGDWORD, "Switch Port 20 Status", ## args) \
	X(IDT_SW_PCI_SWPORT20FCTL, 0x3E488, REGDWORD, "Switch Port 20 Failover Control", ## args) \
	/* Failover capability control and status registers */ \
	X(IDT_SW_PCI_FCAP0CTL,     0x3E500, REGDWORD, "Failover Capability 0 Control", ## args) \
	X(IDT_SW_PCI_FCAP0STS,     0x3E504, REGDWORD, "Failover Capability 0 Status", ## args) \
	X(IDT_SW_PCI_FCAP0TIMER,   0x3E508, REGDWORD, "Failover Capability 0 Watchdog Timer", ## args) \
	X(IDT_SW_PCI_FCAP1CTL,     0x3E520, REGDWORD, "Failover Capability 1 Control", ## args) \
	X(IDT_SW_PCI_FCAP1STS,     0x3E524, REGDWORD, "Failover Capability 1 Status", ## args) \
	X(IDT_SW_PCI_FCAP1TIMER,   0x3E528, REGDWORD, "Failover Capability 1 Watchdog Timer", ## args) \
	X(IDT_SW_PCI_FCAP2CTL,     0x3E540, REGDWORD, "Failover Capability 2 Control", ## args) \
	X(IDT_SW_PCI_FCAP2STS,     0x3E544, REGDWORD, "Failover Capability 2 Status", ## args) \
	X(IDT_SW_PCI_FCAP2TIMER,   0x3E548, REGDWORD, "Failover Capability 2 Watchdog Timer", ## args) \
	X(IDT_SW_PCI_FCAP3CTL,     0x3E560, REGDWORD, "Failover Capability 3 Control", ## args) \
	X(IDT_SW_PCI_FCAP3STS,     0x3E564, REGDWORD, "Failover Capability 3 Status", ## args) \
	X(IDT_SW_PCI_FCAP3TIMER,   0x3E568, REGDWORD, "Failover Capability 3 Watchdog Timer", ## args) \
	/* Protection registers */ \
	X(IDT_SW_PCI_GASAPROT,     0x3E700, REGDWORD, "Global Address Space Access Protection", ## args) \
	X(IDT_SW_PCI_NTMTBLPROT0,  0x3E710, REGDWORD, "Partition 0 NT Mapping Table Protection", ## args) \
	X(IDT_SW_PCI_NTMTBLPROT1,  0x3E714, REGDWORD, "Partition 1 NT Mapping Table Protection", ## args) \
	X(IDT_SW_PCI_NTMTBLPROT2,  0x3E718, REGDWORD, "Partition 2 NT Mapping Table Protection", ## args) \
	X(IDT_SW_PCI_NTMTBLPROT3,  0x3E71C, REGDWORD, "Partition 3 NT Mapping Table Protection", ## args) \
	X(IDT_SW_PCI_NTMTBLPROT4,  0x3E720, REGDWORD, "Partition 4 NT Mapping Table Protection", ## args) \
	X(IDT_SW_PCI_NTMTBLPROT5,  0x3E724, REGDWORD, "Partition 5 NT Mapping Table Protection", ## args) \
	X(IDT_SW_PCI_NTMTBLPROT6,  0x3E728, REGDWORD, "Partition 6 NT Mapping Table Protection", ## args) \
	X(IDT_SW_PCI_NTMTBLPROT7,  0x3E72C, REGDWORD, "Partition 7 NT Mapping Table Protection", ## args) \
	/* Switch Event registers */ \
	X(IDT_SW_PCI_SESTS,        0x3EC00, REGDWORD, "Switch Event Status", ## args) \
	X(IDT_SW_PCI_SEMSK,        0x3EC04, REGDWORD, "Switch Event Mask", ## args) \
	X(IDT_SW_PCI_SEPMSK,       0x3EC08, REGDWORD, "Switch Event Partition Mask", ## args) \
	X(IDT_SW_PCI_SELINKUPSTS,  0x3EC0C, REGDWORD, "Switch Event Link Up Status", ## args) \
	X(IDT_SW_PCI_SELINKUPMSK,  0x3EC10, REGDWORD, "Switch Event Link Up Mask", ## args) \
	X(IDT_SW_PCI_SELINKDNSTS,  0x3EC14, REGDWORD, "Switch Event Link Down Status", ## args) \
	X(IDT_SW_PCI_SELINKDNMSK,  0x3EC18, REGDWORD, "Switch Event Link Down Mask", ## args) \
	X(IDT_SW_PCI_SEFRSTSTS,    0x3EC1C, REGDWORD, "Switch Event Fundamental Reset Status", ## args) \
	X(IDT_SW_PCI_SEFRSTMSK,    0x3EC20, REGDWORD, "Switch Event Fundamental Reset Mask", ## args) \
	X(IDT_SW_PCI_SEHRSTSTS,    0x3EC24, REGDWORD, "Switch Event Hot Reset Status", ## args) \
	X(IDT_SW_PCI_SEHRSTMSK,    0x3EC28, REGDWORD, "Switch Event Hot Reset Mask", ## args) \
	X(IDT_SW_PCI_SEFOVRMSK,    0x3EC2C, REGDWORD, "Switch Event Failover Mask", ## args) \
	X(IDT_SW_PCI_SEGSIGSTS,    0x3EC30, REGDWORD, "Switch Event Global Signal Status", ## args) \
	X(IDT_SW_PCI_SEGSIGMSK,    0x3EC34, REGDWORD, "Switch Event Global Signal Mask", ## args) \
	/* Global Doorbell configuration registers */ \
	X(IDT_SW_PCI_GDBELLSTS,    0x3EC3C, REGDWORD, "NT Global Doorbell Status", ## args) \
	X(IDT_SW_PCI_GODBELLMSK0,  0x3ED00, REGDWORD, "NT Global Outbound Doorbell 0 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK1,  0x3ED04, REGDWORD, "NT Global Outbound Doorbell 1 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK2,  0x3ED08, REGDWORD, "NT Global Outbound Doorbell 2 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK3,  0x3ED0C, REGDWORD, "NT Global Outbound Doorbell 3 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK4,  0x3ED10, REGDWORD, "NT Global Outbound Doorbell 4 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK5,  0x3ED14, REGDWORD, "NT Global Outbound Doorbell 5 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK6,  0x3ED18, REGDWORD, "NT Global Outbound Doorbell 6 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK7,  0x3ED1C, REGDWORD, "NT Global Outbound Doorbell 7 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK8,  0x3ED20, REGDWORD, "NT Global Outbound Doorbell 8 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK9,  0x3ED24, REGDWORD, "NT Global Outbound Doorbell 9 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK10, 0x3ED28, REGDWORD, "NT Global Outbound Doorbell 10 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK11, 0x3ED2C, REGDWORD, "NT Global Outbound Doorbell 11 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK12, 0x3ED30, REGDWORD, "NT Global Outbound Doorbell 12 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK13, 0x3ED34, REGDWORD, "NT Global Outbound Doorbell 13 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK14, 0x3ED38, REGDWORD, "NT Global Outbound Doorbell 14 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK15, 0x3ED3C, REGDWORD, "NT Global Outbound Doorbell 15 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK16, 0x3ED40, REGDWORD, "NT Global Outbound Doorbell 16 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK17, 0x3ED44, REGDWORD, "NT Global Outbound Doorbell 17 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK18, 0x3ED48, REGDWORD, "NT Global Outbound Doorbell 18 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK19, 0x3ED4C, REGDWORD, "NT Global Outbound Doorbell 19 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK20, 0x3ED50, REGDWORD, "NT Global Outbound Doorbell 20 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK21, 0x3ED54, REGDWORD, "NT Global Outbound Doorbell 21 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK22, 0x3ED58, REGDWORD, "NT Global Outbound Doorbell 22 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK23, 0x3ED5C, REGDWORD, "NT Global Outbound Doorbell 23 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK24, 0x3ED60, REGDWORD, "NT Global Outbound Doorbell 24 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK25, 0x3ED64, REGDWORD, "NT Global Outbound Doorbell 25 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK26, 0x3ED68, REGDWORD, "NT Global Outbound Doorbell 26 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK27, 0x3ED6C, REGDWORD, "NT Global Outbound Doorbell 27 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK28, 0x3ED70, REGDWORD, "NT Global Outbound Doorbell 28 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK29, 0x3ED74, REGDWORD, "NT Global Outbound Doorbell 29 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK30, 0x3ED78, REGDWORD, "NT Global Outbound Doorbell 30 Mask", ## args) \
	X(IDT_SW_PCI_GODBELLMSK31, 0x3ED7C, REGDWORD, "NT Global Outbound Doorbell 31 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK0,  0x3ED80, REGDWORD, "NT Global Inbound Doorbell 0 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK1,  0x3ED84, REGDWORD, "NT Global Inbound Doorbell 1 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK2,  0x3ED88, REGDWORD, "NT Global Inbound Doorbell 2 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK3,  0x3ED8C, REGDWORD, "NT Global Inbound Doorbell 3 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK4,  0x3ED90, REGDWORD, "NT Global Inbound Doorbell 4 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK5,  0x3ED94, REGDWORD, "NT Global Inbound Doorbell 5 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK6,  0x3ED98, REGDWORD, "NT Global Inbound Doorbell 6 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK7,  0x3ED9C, REGDWORD, "NT Global Inbound Doorbell 7 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK8,  0x3EDA0, REGDWORD, "NT Global Inbound Doorbell 8 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK9,  0x3EDA4, REGDWORD, "NT Global Inbound Doorbell 9 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK10, 0x3EDA8, REGDWORD, "NT Global Inbound Doorbell 10 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK11, 0x3EDAC, REGDWORD, "NT Global Inbound Doorbell 11 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK12, 0x3EDB0, REGDWORD, "NT Global Inbound Doorbell 12 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK13, 0x3EDB4, REGDWORD, "NT Global Inbound Doorbell 13 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK14, 0x3EDB8, REGDWORD, "NT Global Inbound Doorbell 14 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK15, 0x3EDBC, REGDWORD, "NT Global Inbound Doorbell 15 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK16, 0x3EDC0, REGDWORD, "NT Global Inbound Doorbell 16 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK17, 0x3EDC4, REGDWORD, "NT Global Inbound Doorbell 17 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK18, 0x3EDC8, REGDWORD, "NT Global Inbound Doorbell 18 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK19, 0x3EDCC, REGDWORD, "NT Global Inbound Doorbell 19 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK20, 0x3EDD0, REGDWORD, "NT Global Inbound Doorbell 20 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK21, 0x3EDD4, REGDWORD, "NT Global Inbound Doorbell 21 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK22, 0x3EDD8, REGDWORD, "NT Global Inbound Doorbell 22 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK23, 0x3EDDC, REGDWORD, "NT Global Inbound Doorbell 23 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK24, 0x3EDE0, REGDWORD, "NT Global Inbound Doorbell 24 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK25, 0x3EDE4, REGDWORD, "NT Global Inbound Doorbell 25 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK26, 0x3EDE8, REGDWORD, "NT Global Inbound Doorbell 26 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK27, 0x3EDEC, REGDWORD, "NT Global Inbound Doorbell 27 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK28, 0x3EDF0, REGDWORD, "NT Global Inbound Doorbell 28 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK29, 0x3EDF4, REGDWORD, "NT Global Inbound Doorbell 29 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK30, 0x3EDF8, REGDWORD, "NT Global Inbound Doorbell 30 Mask", ## args) \
	X(IDT_SW_PCI_GIDBELLMSK31, 0x3EDFC, REGDWORD, "NT Global Inbound Doorbell 31 Mask", ## args) \
	/* Switch partition messages control (msgs routing table) */ \
	X(IDT_SW_PCI_SWP0MSGCTL0,  0x3EE00, REGDWORD, "Switch Partition 0 Message Control 0", ## args) \
	X(IDT_SW_PCI_SWP1MSGCTL0,  0x3EE04, REGDWORD, "Switch Partition 1 Message Control 0", ## args) \
	X(IDT_SW_PCI_SWP2MSGCTL0,  0x3EE08, REGDWORD, "Switch Partition 2 Message Control 0", ## args) \
	X(IDT_SW_PCI_SWP3MSGCTL0,  0x3EE0C, REGDWORD, "Switch Partition 3 Message Control 0", ## args) \
	X(IDT_SW_PCI_SWP4MSGCTL0,  0x3EE10, REGDWORD, "Switch Partition 4 Message Control 0", ## args) \
	X(IDT_SW_PCI_SWP5MSGCTL0,  0x3EE14, REGDWORD, "Switch Partition 5 Message Control 0", ## args) \
	X(IDT_SW_PCI_SWP6MSGCTL0,  0x3EE18, REGDWORD, "Switch Partition 6 Message Control 0", ## args) \
	X(IDT_SW_PCI_SWP7MSGCTL0,  0x3EE1C, REGDWORD, "Switch Partition 7 Message Control 0", ## args) \
	X(IDT_SW_PCI_SWP0MSGCTL1,  0x3EE20, REGDWORD, "Switch Partition 0 Message Control 1", ## args) \
	X(IDT_SW_PCI_SWP1MSGCTL1,  0x3EE24, REGDWORD, "Switch Partition 1 Message Control 1", ## args) \
	X(IDT_SW_PCI_SWP2MSGCTL1,  0x3EE28, REGDWORD, "Switch Partition 2 Message Control 1", ## args) \
	X(IDT_SW_PCI_SWP3MSGCTL1,  0x3EE2C, REGDWORD, "Switch Partition 3 Message Control 1", ## args) \
	X(IDT_SW_PCI_SWP4MSGCTL1,  0x3EE30, REGDWORD, "Switch Partition 4 Message Control 1", ## args) \
	X(IDT_SW_PCI_SWP5MSGCTL1,  0x3EE34, REGDWORD, "Switch Partition 5 Message Control 1", ## args) \
	X(IDT_SW_PCI_SWP6MSGCTL1,  0x3EE38, REGDWORD, "Switch Partition 6 Message Control 1", ## args) \
	X(IDT_SW_PCI_SWP7MSGCTL1,  0x3EE3C, REGDWORD, "Switch Partition 7 Message Control 1", ## args) \
	X(IDT_SW_PCI_SWP0MSGCTL2,  0x3EE40, REGDWORD, "Switch Partition 0 Message Control 2", ## args) \
	X(IDT_SW_PCI_SWP1MSGCTL2,  0x3EE44, REGDWORD, "Switch Partition 1 Message Control 2", ## args) \
	X(IDT_SW_PCI_SWP2MSGCTL2,  0x3EE48, REGDWORD, "Switch Partition 2 Message Control 2", ## args) \
	X(IDT_SW_PCI_SWP3MSGCTL2,  0x3EE4C, REGDWORD, "Switch Partition 3 Message Control 2", ## args) \
	X(IDT_SW_PCI_SWP4MSGCTL2,  0x3EE50, REGDWORD, "Switch Partition 4 Message Control 2", ## args) \
	X(IDT_SW_PCI_SWP5MSGCTL2,  0x3EE54, REGDWORD, "Switch Partition 5 Message Control 2", ## args) \
	X(IDT_SW_PCI_SWP6MSGCTL2,  0x3EE58, REGDWORD, "Switch Partition 6 Message Control 2", ## args) \
	X(IDT_SW_PCI_SWP7MSGCTL2,  0x3EE5C, REGDWORD, "Switch Partition 7 Message Control 2", ## args) \
	X(IDT_SW_PCI_SWP0MSGCTL3,  0x3EE60, REGDWORD, "Switch Partition 0 Message Control 3", ## args) \
	X(IDT_SW_PCI_SWP1MSGCTL3,  0x3EE64, REGDWORD, "Switch Partition 1 Message Control 3", ## args) \
	X(IDT_SW_PCI_SWP2MSGCTL3,  0x3EE68, REGDWORD, "Switch Partition 2 Message Control 3", ## args) \
	X(IDT_SW_PCI_SWP3MSGCTL3,  0x3EE6C, REGDWORD, "Switch Partition 3 Message Control 3", ## args) \
	X(IDT_SW_PCI_SWP4MSGCTL3,  0x3EE70, REGDWORD, "Switch Partition 4 Message Control 3", ## args) \
	X(IDT_SW_PCI_SWP5MSGCTL3,  0x3EE74, REGDWORD, "Switch Partition 5 Message Control 3", ## args) \
	X(IDT_SW_PCI_SWP6MSGCTL3,  0x3EE78, REGDWORD, "Switch Partition 6 Message Control 3", ## args) \
	X(IDT_SW_PCI_SWP7MSGCTL3,  0x3EE7C, REGDWORD, "Switch Partition 7 Message Control 3", ## args) \
	/* SerDes's control registers */ \
	X(IDT_SW_PCI_S0CTL,        0x3F000, REGDWORD, "SerDes 0 Control", ## args) \
	X(IDT_SW_PCI_S0TXLCTL0,    0x3F004, REGDWORD, "SerDes 0 Transmitter Lane Control 0", ## args) \
	X(IDT_SW_PCI_S0TXLCTL1,    0x3F008, REGDWORD, "SerDes 0 Transmitter Lane Control 1", ## args) \
	X(IDT_SW_PCI_S0RXEQLCTL,   0x3F010, REGDWORD, "SerDes 0 Receiver Equalization Lane Control", ## args) \
	X(IDT_SW_PCI_S1CTL,        0x3F020, REGDWORD, "SerDes 1 Control", ## args) \
	X(IDT_SW_PCI_S1TXLCTL0,    0x3F024, REGDWORD, "SerDes 1 Transmitter Lane Control 0", ## args) \
	X(IDT_SW_PCI_S1TXLCTL1,    0x3F028, REGDWORD, "SerDes 1 Transmitter Lane Control 1", ## args) \
	X(IDT_SW_PCI_S1RXEQLCTL,   0x3F030, REGDWORD, "SerDes 1 Receiver Equalization Lane Control", ## args) \
	X(IDT_SW_PCI_S2CTL,        0x3F040, REGDWORD, "SerDes 2 Control", ## args) \
	X(IDT_SW_PCI_S2TXLCTL0,    0x3F044, REGDWORD, "SerDes 2 Transmitter Lane Control 0", ## args) \
	X(IDT_SW_PCI_S2TXLCTL1,    0x3F048, REGDWORD, "SerDes 2 Transmitter Lane Control 1", ## args) \
	X(IDT_SW_PCI_S2RXEQLCTL,   0x3F050, REGDWORD, "SerDes 2 Receiver Equalization Lane Control", ## args) \
	X(IDT_SW_PCI_S3CTL,        0x3F060, REGDWORD, "SerDes 3 Control", ## args) \
	X(IDT_SW_PCI_S3TXLCTL0,    0x3F064, REGDWORD, "SerDes 3 Transmitter Lane Control 0", ## args) \
	X(IDT_SW_PCI_S3TXLCTL1,    0x3F068, REGDWORD, "SerDes 3 Transmitter Lane Control 1", ## args) \
	X(IDT_SW_PCI_S3RXEQLCTL,   0x3F070, REGDWORD, "SerDes 3 Receiver Equalization Lane Control", ## args) \
	X(IDT_SW_PCI_S4CTL,        0x3F080, REGDWORD, "SerDes 4 Control", ## args) \
	X(IDT_SW_PCI_S4TXLCTL0,    0x3F084, REGDWORD, "SerDes 4 Transmitter Lane Control 0", ## args) \
	X(IDT_SW_PCI_S4TXLCTL1,    0x3F088, REGDWORD, "SerDes 4 Transmitter Lane Control 1", ## args) \
	X(IDT_SW_PCI_S4RXEQLCTL,   0x3F090, REGDWORD, "SerDes 4 Receiver Equalization Lane Control", ## args) \
	X(IDT_SW_PCI_S5CTL,        0x3F0A0, REGDWORD, "SerDes 5 Control", ## args) \
	X(IDT_SW_PCI_S5TXLCTL0,    0x3F0A4, REGDWORD, "SerDes 5 Transmitter Lane Control 0", ## args) \
	X(IDT_SW_PCI_S5TXLCTL1,    0x3F0A8, REGDWORD, "SerDes 5 Transmitter Lane Control 1", ## args) \
	X(IDT_SW_PCI_S5RXEQLCTL,   0x3F0B0, REGDWORD, "SerDes 5 Receiver Equalization Lane Control", ## args) \
	X(IDT_SW_PCI_S6CTL,        0x3F0C0, REGDWORD, "SerDes 6 Control", ## args) \
	X(IDT_SW_PCI_S6TXLCTL0,    0x3F0C4, REGDWORD, "SerDes 6 Transmitter Lane Control 0", ## args) \
	X(IDT_SW_PCI_S6TXLCTL1,    0x3F0C8, REGDWORD, "SerDes 6 Transmitter Lane Control 1", ## args) \
	X(IDT_SW_PCI_S6RXEQLCTL,   0x3F0D0, REGDWORD, "SerDes 6 Receiver Equalization Lane Control", ## args) \
	X(IDT_SW_PCI_S7CTL,        0x3F0E0, REGDWORD, "SerDes 7 Control", ## args) \
	X(IDT_SW_PCI_S7TXLCTL0,    0x3F0E4, REGDWORD, "SerDes 7 Transmitter Lane Control 0", ## args) \
	X(IDT_SW_PCI_S7TXLCTL1,    0x3F0E8, REGDWORD, "SerDes 7 Transmitter Lane Control 1", ## args) \
	X(IDT_SW_PCI_S7RXEQLCTL,   0x3F0F0, REGDWORD, "SerDes 7 Receiver Equalization Lane Control", ## args) \
	/* GPIO/Hot-plug control registers */ \
	X(IDT_SW_PCI_GPIOFUNC,     0x3F16C, REGDWORD, "General Purpose I/O Function", ## args) \
	X(IDT_SW_PCI_GPIOAFSEL,    0x3F170, REGDWORD, "General Purpose I/O Alternate Function Select", ## args) \
	X(IDT_SW_PCI_GPIOCFG,      0x3F174, REGDWORD, "General Purpose I/O Configuration", ## args) \
	X(IDT_SW_PCI_GPIOD,        0x3F178, REGDWORD, "General Purpose I/O Data", ## args) \
	X(IDT_SW_PCI_HPCFGCTL,     0x3F17C, REGDWORD, "Hot-Plug Configuration Control", ## args) \
	/* SMBus related registers */ \
	X(IDT_SW_PCI_SMBUSSTS,     0x3F188, REGDWORD, "SMBus Status", ## args) \
	X(IDT_SW_PCI_SMBUSCTL,     0x3F18C, REGDWORD, "SMBus Control", ## args) \
	X(IDT_SW_PCI_EEPROMINTF,   0x3F190, REGDWORD, "Serial EEPROM Interface", ## args) \
	/* SMBus IO expanders */ \
	X(IDT_SW_PCI_IOEXPADDR0,   0x3F198, REGDWORD, "SMBus I/O Expander Address 0", ## args) \
	X(IDT_SW_PCI_IOEXPADDR1,   0x3F19C, REGDWORD, "SMBus I/O Expander Address 1", ## args) \
	X(IDT_SW_PCI_IOEXPADDR2,   0x3F1A0, REGDWORD, "SMBus I/O Expander Address 2", ## args) \
	X(IDT_SW_PCI_IOEXPADDR3,   0x3F1A4, REGDWORD, "SMBus I/O Expander Address 3", ## args) \
	X(IDT_SW_PCI_IOEXPADDR4,   0x3F1A8, REGDWORD, "SMBus I/O Expander Address 4", ## args) \
	X(IDT_SW_PCI_IOEXPADDR5,   0x3F1AC, REGDWORD, "SMBus I/O Expander Address 5", ## args) \
	/* General Purpose Events registers */ \
	X(IDT_SW_PCI_GPECTL,       0x3F1B0, REGDWORD, "General Purpose Event Control", ## args) \
	X(IDT_SW_PCI_GPESTS,       0x3F1B4, REGDWORD, "General Purpose Event Status", ## args) \
	/* Temperature sensor */ \
	X(IDT_SW_PCI_TMPCTL,       0x3F1D4, REGDWORD, "Temperature Sensor Control", ## args) \
	X(IDT_SW_PCI_TMPSTS,       0x3F1D8, REGDWORD, "Temperature Sensor Status", ## args) \
	X(IDT_SW_PCI_TMPALARM,     0x3F1DC, REGDWORD, "Temperature Sensor Alarm", ## args) \
	X(IDT_SW_PCI_TMPADJ,       0x3F1E0, REGDWORD, "Temperature Sensor Adjustment", ## args) \
	X(IDT_SW_PCI_TSSLOPE,      0x3F1E4, REGDWORD, "Temperature Sensor Slope", ## args) \
	/* SMBus Configuration Block header log */ \
	X(IDT_SW_PCI_SMBUSCBHL,    0x3F1E8, REGDWORD, "SMBus Configuration Block Header Log", ## args)

/*
 * Enumeration of the IDT PCIe-switch NT registers. It's not actual
 * addresses or offsets, but the numerated names, which are used to find the
 * necessary values from the tables above. Consenquently the switch-case
 * shall help to retrieve all the information for the IO operations. Of course,
 * we are sure the compiler will translate that statement into the jump table
 * pattern.
 *
 * NOTE 1) The IDT PCIe-switch internal data is littel-endian
 *      so it must be taken into account in the driver
 *      internals.
 *      2) Additionally the registers should be accessed either
 *      with byte-enables corresponding to their native size or
 *      a size of one DWORD
 *      3) Global registers registers can be accessed by the
 *      GASAADDR and GASADATA registers of NT-functions only
 */
enum idt_ntb_cfgreg {
	IDT_NT_CFGREGS(PAIR_ID_ENUM)
	IDT_NTB_CFGREGS_SPLIT,
	IDT_SW_CFGREGS(PAIR_ID_ENUM)
	IDT_NTB_CFGREGS_END
};

/*
 * IDT PCIe-switch register type. It's vital that the types are assigned
 * with 0 and 1 since those values are used to determine the registers IO
 * context
 * @IDT_NT_REGTYPE: NT-function register accessed using the mmio
 * @IDT_SW_REGTYPE: IDT PCIe-switch Gobal register accessed using GASA regs
 */
enum idt_ntb_regtype {
	IDT_NT_REGTYPE = 0,
	IDT_SW_REGTYPE = 1
};

/*
 * R/W registers operation context structure
 * @writereg:	Register write function
 * @readreg:	Register read function
 * @iolock:	Spin lock of the registers access
 */
struct idt_ntb_regctx {
	void (*writereg)(void __iomem *cfg_mmio, const ptrdiff_t regoffset,
			  const enum idt_ntb_regsize regsize, const u32 val);
	u32  (*readreg)(void __iomem *cfg_mmio, const ptrdiff_t regoffset,
			 const enum idt_ntb_regsize regsize);
	spinlock_t iolock;
};

#endif /* NTB_HW_IDT_REGMAP_H */
