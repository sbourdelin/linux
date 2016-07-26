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

/*
 *           NOTE of the IDT PCIe-switch NT-function driver design.
 * Here is presented some lirics about the NT-functions of the IDT PCIe-switch
 * and the driver concept.
 *
 * There are a lot of different architectures or configurations the IDT
 * PCIe-switch can be placed into, like NT Bridge-to-Bridge, Port-to-Port,
 * Ports-to-Ports, Port-to-Ports, etc. But there is always BUT! Here it is.
 * But the problem is that the PCIe-switch resources are not balanced enough
 * to create efficient, the most comprehensive driver for Ports-to-Ports
 * configuration. Here is what each IDT PCIe-switch have (IDT family of
 * PCIe-switch solutions):
 * - up to 24 Memory Windows per each port (incredibly a lot comparing to the
 *   Intel and AMD controllers)
 * - one 32 bits Doorbell register shared amongst all the ports (Why IDT, why
 *   would you do that? Why so few?!)
 * - 4 Message registers per each port (IDT, thanks at least for that...)
 * - No Scratchpad registers (Surprise, huh?!)
 *
 * Since there are no scratchpad registers integrated in the IDT family PCI
 * ExpressR switches, the tradition synchronous Linux NTB device can't be
 * implemented (unlike Intel and AMD controllers, that are strictly synchronous).
 * Instead the Messaging mechanism should be used to exchange the necessary
 * informatin among the NT-functions. It leads to the asynchronous
 * interface since there is no easy way to pass the address of the locally
 * allocated shared memory window to the opposite NT-function. It can only be
 * done by sending a message, which must be correcly handled by a peer. If one
 * is looking for strictly synchronous solutions, then it's better to use Intel
 * and AMD controllers. Regarding the IDT PreciseTM family of PCI ExpressR
 * switches, they actually support both synchronous (scratchpads) and
 * asynchronous (message registers) interfaces, but there is no suitable driver
 * to use them in Linux.
 *
 * Lets get back to the actual driver architecture. Since there are no enough
 * doorbell registers and after a lot of thoughts of the possible sidewalks to
 * bypass the PCIe-switch limitations we came to the conclusion, that the best
 * architecture of the driver using as much resources as possible would be the
 * Port-to-Port/Port-to-Ports one. Shortly speaking it implies the only one
 * NT-function being able to communicate with all the other NT-functions
 * simultaniously. Suppose there are eight ports working as NT-bridge, then the
 * Primary port would have 7 devices on the NTB bus, but the Secondary ports
 * will expose just one device. As one can see it also perfectly fits the
 * Primary-Secondary topology of the Linux NTB bus. The NTSDATA registers must
 * be preinitialized with the corresponding Primary side port numbers. It is the
 * way how the NTB topology can be configurated. For instance, suppose there are
 * only two NT-functions enabled on the IDT PCIe-switch ports 0 and 2, where
 * port 2 is chosen to be the primary one. Then all NTSDATA of the both
 * NT-functions must be preinitialized with value 2. Similarly the topology with
 * several Primary ports can be created.
 *
 *                           Howto study the code below.
 * Here is the content of the driver:
 * 1. IDT PCIe-switch registers IO-functions
 * 2. Synchronization methods: atomic queue ops
 * 3. Link status operations
 * 4. Memory Window subsystem
 * 5. Doorbells subsystem
 * 6. Messaging subsystem
 * 7. IRQ-related functions
 * 8. NTB bus initialization
 * 9. IDT NT-functions topology
 * 10. Basic initialization functions
 * 11. DebugFS callback functions
 * 12. PCI bus callback functions
 *
 * I would recommend to start from the chapter "1. IDT PCIe-switch registers
 * IO-functions". Since there are a lot of registers must be initialized before
 * the switch starts working, it's better to have the register addresses and
 * the corresponding values being collected at some structured table.
 * Particulary one can find these tables in ntb_hw_idt_regmap.h file as the set
 * of preprocessor macro-functions. Regarding the chapter 1 in this file, it
 * resides the basic functions used to create the NT-functions and Switch Global
 * registers table and the registers fields table. There are also r/w functions
 * determined in there.
 *
 * Since there are list structures used to handle in and out messages, then
 * there has to be managed synchronous access to those lists. Therefore the
 * operations with message queues are made atomic in chapter "2. Synchronization
 * methods: atomic queue ops".
 *
 * Then I would get stright to the chapter "12. PCI bus callback functions",
 * which perform the algorithm of the PCI-bus device basic initialzation.
 * Particulary it checks whether the kernel supports IDT PCIe-switch NTB
 * devices, allocates the necessary structures, initialize the PCI-related
 * fields, scans the IDT PCIe-switch NT-functions topology, adds all the
 * available peers, initalizes doorbells, memory windows and messaging
 * subsystem, starts link polling work-thread, initialize the interrupt
 * handlers and finally registers the NTB devices on the NTB linux kernel bus.
 *
 * The basic PCI-bus device initialization and data structures allocation are
 * performed by means of methods defined in the chapter "10. Basic
 * initialization functions". NTB topology scanning is made by function from
 * the chapter "9. IDT NT-functions topology".
 *
 * The NTB basic interfaces like Link event handlers, memory windows, doorbells
 * and messages subsystems are described in the chapters 3 - 6 with corresponding
 * titles. They traditionally consist of helpers, initializing/deinitializing
 * functions and particular NTB devices kernel driver methods. These kernel
 * driver methods - are callback functions used to register the new devices on
 * the linux kernel NTB bus defined in the chapter "8. NTB bus initialization".
 */

/*#define DEBUG*/

#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/sizes.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/ntb.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/pci.h>
#include <linux/aer.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/debugfs.h>

#include "ntb_hw_idt.h"
#include "ntb_hw_idt_regmap.h"
#include "ntb_hw_idt_quirks.h"

#define NTB_NAME	"ntb_hw_idt"
#define NTB_DESC	"IDT PCI-E Non-Transparent Bridge Driver"
#define NTB_VER		"1.0"
#define NTB_IRQNAME	"idt_ntb_irq"
#define NTB_WQNAME	"idt_ntb_wq"
#define NTB_CACHENAME	"idt_ntb_cache"

MODULE_DESCRIPTION(NTB_DESC);
MODULE_VERSION(NTB_VER);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("T-platforms");

/*
 * Wrapper dev_err/dev_warn/dev_info/dev_dbg macros
 */
#define dev_err_ndev(ndev, args...) \
	dev_err(to_dev_ndev(ndev), ## args)
#define dev_err_data(data, args...) \
	dev_err(to_dev_data(data), ## args)
#define dev_warn_ndev(ndev, args...) \
	dev_warn(to_dev_ndev(ndev), ## args)
#define dev_warn_data(data, args...) \
	dev_warn(to_dev_data(data), ## args)
#define dev_info_ndev(ndev, args...) \
	dev_info(to_dev_ndev(ndev), ## args)
#define dev_info_data(data, args...) \
	dev_info(to_dev_data(data), ## args)
#define dev_dbg_ndev(ndev, args...) \
	dev_dbg(to_dev_ndev(ndev), ## args)
#define dev_dbg_data(data, args...) \
	dev_dbg(to_dev_data(data), ## args)

/*
 * NT Endpoint ports table with the corresponding pcie link status, signal data,
 * control and status registers
 */
static struct idt_ntb_port portdata_tbl[IDT_NTB_MAXPORTS_CNT] = {
/*0*/	{IDT_SW_PCI_NTP0_CMD,       IDT_SW_PCI_NTP0_PCIELSTS,
	 IDT_SW_PCI_NTP0_NTSDATA,   IDT_SW_PCI_NTP0_NTGSIGNAL,
	 IDT_SW_PCI_SWPORT0CTL,     IDT_SW_PCI_SWPORT0STS},
/*1*/	{0},
/*2*/	{IDT_SW_PCI_NTP2_CMD,       IDT_SW_PCI_NTP2_PCIELSTS,
	 IDT_SW_PCI_NTP2_NTSDATA,   IDT_SW_PCI_NTP2_NTGSIGNAL,
	 IDT_SW_PCI_SWPORT2CTL,     IDT_SW_PCI_SWPORT2STS},
/*3*/	{0},
/*4*/	{IDT_SW_PCI_NTP4_CMD,       IDT_SW_PCI_NTP4_PCIELSTS,
	 IDT_SW_PCI_NTP4_NTSDATA,   IDT_SW_PCI_NTP4_NTGSIGNAL,
	 IDT_SW_PCI_SWPORT4CTL,     IDT_SW_PCI_SWPORT4STS},
/*5*/	{0},
/*6*/	{IDT_SW_PCI_NTP6_CMD,       IDT_SW_PCI_NTP6_PCIELSTS,
	 IDT_SW_PCI_NTP6_NTSDATA,   IDT_SW_PCI_NTP6_NTGSIGNAL,
	 IDT_SW_PCI_SWPORT6CTL,     IDT_SW_PCI_SWPORT6STS},
/*7*/	{0},
/*8*/	{IDT_SW_PCI_NTP8_CMD,       IDT_SW_PCI_NTP8_PCIELSTS,
	 IDT_SW_PCI_NTP8_NTSDATA,   IDT_SW_PCI_NTP8_NTGSIGNAL,
	 IDT_SW_PCI_SWPORT8CTL,     IDT_SW_PCI_SWPORT8STS},
/*9*/	{0},
/*10*/	{0},
/*11*/	{0},
/*12*/	{IDT_SW_PCI_NTP12_CMD,      IDT_SW_PCI_NTP12_PCIELSTS,
	 IDT_SW_PCI_NTP12_NTSDATA,  IDT_SW_PCI_NTP12_NTGSIGNAL,
	 IDT_SW_PCI_SWPORT12CTL,    IDT_SW_PCI_SWPORT12STS},
/*13*/	{0},
/*14*/	{0},
/*15*/	{0},
/*16*/	{IDT_SW_PCI_NTP16_CMD,      IDT_SW_PCI_NTP16_PCIELSTS,
	 IDT_SW_PCI_NTP16_NTSDATA,  IDT_SW_PCI_NTP16_NTGSIGNAL,
	 IDT_SW_PCI_SWPORT16CTL,    IDT_SW_PCI_SWPORT16STS},
/*17*/	{0},
/*18*/	{0},
/*19*/	{0},
/*20*/	{IDT_SW_PCI_NTP20_CMD,      IDT_SW_PCI_NTP20_PCIELSTS,
	 IDT_SW_PCI_NTP20_NTSDATA,  IDT_SW_PCI_NTP20_NTGSIGNAL,
	 IDT_SW_PCI_SWPORT20CTL,    IDT_SW_PCI_SWPORT20STS},
/*21*/	{0},
/*22*/	{0},
/*23*/	{0}
};

/*
 * IDT PCIe-switch partitions table with the corresponding control, status
 * and messages control registers
 */
static struct idt_ntb_part partdata_tbl[IDT_NTB_MAXPARTS_CNT] = {
/*0*/	{ IDT_SW_PCI_SWPART0CTL, IDT_SW_PCI_SWPART0STS,
	  {IDT_SW_PCI_SWP0MSGCTL0, IDT_SW_PCI_SWP0MSGCTL1,
	   IDT_SW_PCI_SWP0MSGCTL2, IDT_SW_PCI_SWP0MSGCTL3} },
/*1*/	{ IDT_SW_PCI_SWPART1CTL, IDT_SW_PCI_SWPART1STS,
	  {IDT_SW_PCI_SWP1MSGCTL0, IDT_SW_PCI_SWP1MSGCTL1,
	   IDT_SW_PCI_SWP1MSGCTL2, IDT_SW_PCI_SWP1MSGCTL3} },
/*2*/	{ IDT_SW_PCI_SWPART2CTL, IDT_SW_PCI_SWPART2STS,
	  {IDT_SW_PCI_SWP2MSGCTL0, IDT_SW_PCI_SWP2MSGCTL1,
	   IDT_SW_PCI_SWP2MSGCTL2, IDT_SW_PCI_SWP2MSGCTL3} },
/*3*/	{ IDT_SW_PCI_SWPART3CTL, IDT_SW_PCI_SWPART3STS,
	  {IDT_SW_PCI_SWP3MSGCTL0, IDT_SW_PCI_SWP3MSGCTL1,
	   IDT_SW_PCI_SWP3MSGCTL2, IDT_SW_PCI_SWP3MSGCTL3} },
/*4*/	{ IDT_SW_PCI_SWPART4CTL, IDT_SW_PCI_SWPART4STS,
	  {IDT_SW_PCI_SWP4MSGCTL0, IDT_SW_PCI_SWP4MSGCTL1,
	   IDT_SW_PCI_SWP4MSGCTL2, IDT_SW_PCI_SWP4MSGCTL3} },
/*5*/	{ IDT_SW_PCI_SWPART5CTL, IDT_SW_PCI_SWPART5STS,
	  {IDT_SW_PCI_SWP5MSGCTL0, IDT_SW_PCI_SWP5MSGCTL1,
	   IDT_SW_PCI_SWP5MSGCTL2, IDT_SW_PCI_SWP5MSGCTL3} },
/*6*/	{ IDT_SW_PCI_SWPART6CTL, IDT_SW_PCI_SWPART6STS,
	  {IDT_SW_PCI_SWP6MSGCTL0, IDT_SW_PCI_SWP6MSGCTL1,
	   IDT_SW_PCI_SWP6MSGCTL2, IDT_SW_PCI_SWP6MSGCTL3} },
/*7*/	{ IDT_SW_PCI_SWPART7CTL, IDT_SW_PCI_SWPART7STS,
	  {IDT_SW_PCI_SWP7MSGCTL0, IDT_SW_PCI_SWP7MSGCTL1,
	   IDT_SW_PCI_SWP7MSGCTL2, IDT_SW_PCI_SWP7MSGCTL3} }
};

/*
 * DebugFS directory to place the driver debug file
 */
static struct dentry *dbgfs_topdir;

/*===========================================================================
 *                1. IDT PCIe-switch registers IO-functions
 *===========================================================================*/

static void __idt_nt_writereg(void __iomem *cfg_mmio, const ptrdiff_t regoffset,
			      const enum idt_ntb_regsize regsize, const u32 val);
static u32 __idt_nt_readreg(void __iomem *cfg_mmio, const ptrdiff_t regoffset,
			    const enum idt_ntb_regsize regsize);
static void __idt_sw_writereg(void __iomem *cfg_mmio, const ptrdiff_t regoffset,
			      const enum idt_ntb_regsize regsize, const u32 val);
static u32 __idt_sw_readreg(void __iomem *cfg_mmio, const ptrdiff_t regoffset,
			    const enum idt_ntb_regsize regsize);

/*
 * Registers IO contexts to perform the r/w operations either with NT-function
 * registers or with the PCIe-switch Global registers. The context is chosen
 * by the register type "enum idt_ntb_regtype"
 */
static struct idt_ntb_regctx regctx[2] = {
	{.writereg = __idt_nt_writereg, .readreg = __idt_nt_readreg,
	 .iolock = __SPIN_LOCK_UNLOCKED(iolock)},
	{.writereg = __idt_sw_writereg, .readreg = __idt_sw_readreg,
	 .iolock = __SPIN_LOCK_UNLOCKED(iolock)}
};

/*
 * Internal function to set the value bits of a variable
 */
static inline u32 idt_ntb_setbits(u32 var, u32 mask, unsigned char offset, u32 val)
{
	return (var & ~(mask << offset)) | ((val & mask) << offset);
}

/*
 * Internal function to retrieve the value bits of a variable
 */
static inline u32 idt_ntb_getbits(u32 var, u32 mask, unsigned char offset)
{
	return (var >> offset) & mask;
}

/*
 * Retrieve the register type, address and size by the passed enumerated ID
 * NOTE Compiler should produce the jump table for the subsequent switch-case
 *      statement which gives just simple o(1) complexity
 */
static int idt_ntb_regparams(const enum idt_ntb_cfgreg reg,
			     enum idt_ntb_regtype *type, ptrdiff_t *offset,
			     enum idt_ntb_regsize *size, const char **desc)
{
	const char *tmpdesc;

	/* Determine the register type */
	*type = (IDT_NTB_CFGREGS_SPLIT > reg) ? IDT_NT_REGTYPE : IDT_SW_REGTYPE;

	/* Retrieve the register parameters by the enumerated ID */
	switch (reg) {
	IDT_NT_CFGREGS(PAIR_REGID_ACCESS, *offset, *size, tmpdesc)
	IDT_SW_CFGREGS(PAIR_REGID_ACCESS, *offset, *size, tmpdesc)
	default :
		/* Got invalid register ID */
		BUG();
		return -EINVAL;
	}

	/* Return the pointer to the string with the register description
	 * only if the passed pointer isn't NULL*/
	if (NULL != desc) {
		*desc = tmpdesc;
	}

	return SUCCESS;
}

/*
 * Retrieve the registers fields parameters like the register id, mask
 * and offset
 * NOTE Compiler should produce the jump table for the subsequent switch-case
 *      statement which gives just simple o(1) complexity
 */
static int idt_ntb_fldparams(const enum idt_ntb_regfld fld,
			     enum idt_ntb_cfgreg *reg,
			     u32 *mask, unsigned char *offset)
{
	/* Retrieve the field parameters by the enumerated ID */
	switch (fld) {
	IDT_NT_REGFLDS(PAIR_FLDID_ACCESS, *reg, *mask, *offset)
	IDT_SW_REGFLDS(PAIR_FLDID_ACCESS, *reg, *mask, *offset)
	default :
		/* Got invalid register field ID */
		BUG();
		return -EINVAL;
	}
	return SUCCESS;
}

/*
 * Set the corresponding field of the passed variable
 */
static void idt_ntb_writefld_var(u32 *var, const enum idt_ntb_regfld fld,
				 const u32 val)
{
	enum idt_ntb_cfgreg reg;
	unsigned char bitoffset;
	u32 mask;

	/* Retrieve the field parameters */
	idt_ntb_fldparams(fld, &reg, &mask, &bitoffset);

	/* Init the corresponding bits of the passed variable */
	*var = idt_ntb_setbits(*var, mask, bitoffset, val);
}

/*
 * Get the corresponding field of the passed variable
 */
static u32 idt_ntb_readfld_var(u32 var, const enum idt_ntb_regfld fld)
{
	enum idt_ntb_cfgreg reg;
	unsigned char bitoffset;
	u32 mask;

	/* Retrieve the field parameters */
	idt_ntb_fldparams(fld, &reg, &mask, &bitoffset);

	/* Retrieve the corresponding field of the variable */
	return idt_ntb_getbits(var, mask, bitoffset);
}

/*
 * NT-function registers basic write function
 *
 * WARNING! Our target platform is Big Endian, but PCI registers are always
 *          Little endian. So corresponding write{w,l} operations must have
 *          embedded endiannes conversion. If your platform doesn't have it,
 *          the driver won't properly work.
 */
static void __idt_nt_writereg(void __iomem *cfg_mmio, const ptrdiff_t regoffset,
			      const enum idt_ntb_regsize regsize, const u32 val)
{
	/* Perform fast IO operation */
	switch (regsize) {
	case REGBYTE:
		writeb((u8)val, cfg_mmio + regoffset);
		break;
	case REGWORD:
		writew((u16)val, cfg_mmio + regoffset);
		break;
	case REGDWORD:
		writel((u32)val, cfg_mmio + regoffset);
		break;
	default:
		/* Invalid register size was retrieved */
		BUG();
		break;
	}
}

/*
 * NT-function registers basic read function
 *
 * WARNING! Our target platform is Big Endian, but PCI registers are always
 *          Little endian. So corresponding read{w,l} operations must have
 *          embedded endiannes conversion. If your platform doesn't have it,
 *          the driver won't properly work.
 */
static u32 __idt_nt_readreg(void __iomem *cfg_mmio, const ptrdiff_t regoffset,
			    const enum idt_ntb_regsize regsize)
{
	u32 retval;

	/* Perform fast IO operation */
	switch (regsize) {
	case REGBYTE:
		retval = readb(cfg_mmio + regoffset);
		break;
	case REGWORD:
		retval = readw(cfg_mmio + regoffset);
		break;
	case REGDWORD:
		retval = readl(cfg_mmio + regoffset);
		break;
	default:
		/* Invalid register size was retrieved */
		BUG();
		break;
	}

	return retval;
}

/*
 * IDT PCIe-switch Global registers basic write function
 *
 * WARNING! Our target platform is Big Endian, but PCI registers are always
 *          Little endian. So corresponding write{w,l} operations must have
 *          embedded endiannes conversion. If your platform doesn't have it,
 *          the driver won't properly work.
 *          In addition the GASA* registers support the 4 bytes R/W operations
 *          so the data must be correspondingly shifted
 */
static void __idt_sw_writereg(void __iomem *cfg_mmio, const ptrdiff_t regoffset,
			      const enum idt_ntb_regsize regsize, const u32 val)
{
	u32 data, fldmask;
	unsigned char fldoffset;

	/* Post the IDT PCIe-switch register offset first */
	writel((u32)regoffset, cfg_mmio + GASAADDR_OFFSET);

	/* Read the data of the passed register, which offset is aligned with
	 * two last bits by the GASAADDR register */
	data = readl(cfg_mmio + GASADATA_OFFSET);

	/* Alter the corresponding field of the data with the passed value */
	fldmask = GENMASK(BITS_PER_BYTE * regsize - 1, 0);
	fldoffset = BITS_PER_BYTE * (regoffset & 0x3);
	data = idt_ntb_setbits(data, fldmask, fldoffset, val);

	/* Whatever the size of the register is, just write the value to the
	 * data register */
	writel(data, cfg_mmio + GASADATA_OFFSET);
}

/*
 * IDT PCIe-switch Global registers basic read function
 */
static u32 __idt_sw_readreg(void __iomem *cfg_mmio, const ptrdiff_t regoffset,
			    const enum idt_ntb_regsize regsize)
{
	u32 data, fldmask;
	unsigned char fldoffset;

	/* Post the IDT PCIe-switch register offset first */
	writel((u32)regoffset, cfg_mmio + GASAADDR_OFFSET);

	/* Read the data of the passed register, which offset is aligned with
	 * two last bits by the GASAADDR register */
	data = readl(cfg_mmio + GASADATA_OFFSET);

	/* Alter the corresponding field of the data with the passed value */
	fldmask = GENMASK(BITS_PER_BYTE * regsize - 1, 0);
	fldoffset = BITS_PER_BYTE * (regoffset & 0x3);
	data = idt_ntb_getbits(data, fldmask, fldoffset);

	/* Return the corresponding field of the register */
	return data;
}

/*
 * General function to perform the write operation to the register
 */
static void idt_ntb_writereg(void __iomem *cfg_mmio,
			     const enum idt_ntb_cfgreg reg, const u32 val)
{
	struct idt_ntb_regctx *curctx;
	enum idt_ntb_regtype regtype;
	ptrdiff_t regoffset;
	enum idt_ntb_regsize regsize;
	unsigned long irqflags;

	/* Retrieve the register type, offset and size */
	idt_ntb_regparams(reg, &regtype, &regoffset, &regsize, NULL);

	/* Get the current register context */
	curctx = &regctx[regtype];

	/* Perform fast write operation */
	spin_lock_irqsave(&curctx->iolock, irqflags);
	curctx->writereg(cfg_mmio, regoffset, regsize, val);
	spin_unlock_irqrestore(&curctx->iolock, irqflags);
}

/*
 * General function to perform the read operation from the register
 */
static u32 idt_ntb_readreg(void __iomem *cfg_mmio, const enum idt_ntb_cfgreg reg)
{
	struct idt_ntb_regctx *curctx;
	enum idt_ntb_regtype regtype;
	ptrdiff_t regoffset;
	enum idt_ntb_regsize regsize;
	unsigned long irqflags;
	u32 val;

	/* Retrieve the register type, offset and size */
	idt_ntb_regparams(reg, &regtype, &regoffset, &regsize, NULL);

	/* Get the current register context */
	curctx = &regctx[regtype];

	/* Perform fast read operation */
	spin_lock_irqsave(&curctx->iolock, irqflags);
	val = curctx->readreg(cfg_mmio, regoffset, regsize);
	spin_unlock_irqrestore(&curctx->iolock, irqflags);

	return val;
}

/*
 * General function to perform the write operation to the field of the register
 */
static void idt_ntb_writefld_mem(void __iomem *cfg_mmio,
				 const enum idt_ntb_regfld fld, const u32 val)
{
	struct idt_ntb_regctx *curctx;
	enum idt_ntb_cfgreg reg;
	enum idt_ntb_regsize regsize;
	ptrdiff_t regoffset;
	unsigned char bitoffset;
	u32 mask, curval;
	enum idt_ntb_regtype regtype;
	unsigned long irqflags;

	/* Retrieve the field parameters */
	idt_ntb_fldparams(fld, &reg, &mask, &bitoffset);

	/* Retrieve the register offset and size */
	idt_ntb_regparams(reg, &regtype, &regoffset, &regsize, NULL);

	/* Get the current register set context */
	curctx = &regctx[regtype];

	/* Perform fast IO operations */
	spin_lock_irqsave(&curctx->iolock, irqflags);
	/* Retrieve the current value of the register */
	curval = curctx->readreg(cfg_mmio, regoffset, regsize);
	/* Set the corresponding bits in there */
	curval = idt_ntb_setbits(curval, mask, bitoffset, val);
	/* Write the register value back */
	curctx->writereg(cfg_mmio, regoffset, regsize, val);
	/* The critical section is over */
	spin_unlock_irqrestore(&curctx->iolock, irqflags);
}

/*
 * General function to perform the read operation from the field of the register
 */
static u32 idt_ntb_readfld_mem(void __iomem *cfg_mmio,
			       const enum idt_ntb_regfld fld)
{
	struct idt_ntb_regctx *curctx;
	enum idt_ntb_cfgreg reg;
	enum idt_ntb_regsize regsize;
	ptrdiff_t regoffset;
	unsigned char bitoffset;
	u32 mask, curval;
	enum idt_ntb_regtype regtype;
	unsigned long irqflags;

	/* Retrieve the field parameters */
	idt_ntb_fldparams(fld, &reg, &mask, &bitoffset);

	/* Retrieve the register offset and size */
	idt_ntb_regparams(reg, &regtype, &regoffset, &regsize, NULL);

	/* Get the current register set context */
	curctx = &regctx[regtype];

	/* Perform fast IO operations */
	spin_lock_irqsave(&curctx->iolock, irqflags);
	/* Retrieve the current value of the register */
	curval = curctx->readreg(cfg_mmio, regoffset, regsize);
	/* The critical section is over */
	spin_unlock_irqrestore(&curctx->iolock, irqflags);

	return idt_ntb_getbits(curval, mask, bitoffset);
}

/*===========================================================================
 *                2. Synchronization methods: atomic queue ops
 *===========================================================================*/

/*
 * Initialize the atomic queue structure
 */
static inline void atomic_queue_init(queue_atomic_t *queue)
{
	/* Init the queue head */
	INIT_LIST_HEAD(&queue->head);

	/* Initialize the spin lock protecting the queue head */
	spin_lock_init(&queue->lock);
}

/*
 * Add item to the atomic queue at the first position
 */
static inline void atomic_queue_add(queue_atomic_t *queue,
				    struct list_head *new)
{
	unsigned long irqflags;

	/* Lock the list add operation */
	spin_lock_irqsave(&queue->lock, irqflags);
	list_add(new, &queue->head);
	spin_unlock_irqrestore(&queue->lock, irqflags);
}

/*
 * Add item to the atomic queue tail
 */
static inline void atomic_queue_add_tail(queue_atomic_t *queue,
					 struct list_head *new)
{
	unsigned long irqflags;

	/* Lock the list add tail operation */
	spin_lock_irqsave(&queue->lock, irqflags);
	list_add_tail(new, &queue->head);
	spin_unlock_irqrestore(&queue->lock, irqflags);
}

/*
 * Get the very first entry from the atomic queue
 */
static inline struct list_head *atomic_queue_get(queue_atomic_t *queue)
{
	struct list_head *entry;
	unsigned long irqflags;

	/* Lock the list entry delete operation */
	spin_lock_irqsave(&queue->lock, irqflags);
	if (!list_empty(&queue->head)) {
		entry = queue->head.next;
		list_del(entry);
	} else /* if (entry != &queue->head) */ {
		entry = NULL;
	}
	spin_unlock_irqrestore(&queue->lock, irqflags);

	return entry;
}

/*
 * Check whether the atomic queue is empty
 */
static inline bool atomic_queue_empty(queue_atomic_t *queue)
{
	unsigned long irqflags;
	bool ret;

	/* Lock the list empty operation */
	spin_lock_irqsave(&queue->lock, irqflags);
	ret = list_empty(&queue->head);
	spin_unlock_irqrestore(&queue->lock, irqflags);

	return ret;
}

/*===========================================================================
 *                         3. Link status operations
 *===========================================================================*/

/*
 * Effectively enable the NTB link.
 *
 * From the moment of return from this function the inter-partition
 * communications are enabled as well as translating Request and Complition TLPs.
 * This function is called by the Primary side on the initialization phase. The
 * Secondary ports can invoke it by calling the ntb_link_enable() callback.
 */
static void idt_ntb_link_effective_enable(struct idt_ntb_data *pdata)
{
	void __iomem *cfg = pdata->cfg_mmio;
	u32 ntctl = 0, reqid, ntmtbldata = 0;

	/* Retrieve the current complex Requester ID (Bus:Device:Function) */
	reqid = idt_ntb_readfld_mem(cfg, IDT_NT_MTBL_REQID);

	/* Set the corresponding NT Mapping table entry of port partition index
	 * with the data to perform the Request ID translation */
	idt_ntb_writefld_var(&ntmtbldata, IDT_NT_MTBL_BDF, reqid);
	idt_ntb_writefld_var(&ntmtbldata, IDT_NT_MTBL_PART, (u32)pdata->part);
	idt_ntb_writefld_var(&ntmtbldata, IDT_NT_MTBL_VALID, ON);
	idt_ntb_writereg(cfg, IDT_NT_PCI_NTMTBLADDR, (u32)pdata->part);
	idt_ntb_writereg(cfg, IDT_NT_PCI_NTMTBLDATA, ntmtbldata);

	/* Enable the ID protection and Completion TLPs translation */
	idt_ntb_writefld_var(&ntctl, IDT_NT_IDPROTDIS, OFF);
	idt_ntb_writefld_var(&ntctl, IDT_NT_CPEN, ON);
	idt_ntb_writereg(cfg, IDT_NT_PCI_NTCTL, ntctl);

	/* Enable the bus mastering, which effectively enables the Request TLPs
	 * translation and MSI IRQs generation */
	pci_set_master(pdata->pdev);

	/* The ndevs->lnk_sts variable is going to change in the work thread */
}

/*
 * Effectively disable the NTB link.
 *
 * From the moment of return from this function the inter-partition
 * communications are disabled.
 */
static void idt_ntb_link_effective_disable(struct idt_ntb_data *pdata)
{
	void __iomem *cfg = pdata->cfg_mmio;

	/* Disable the bus mastering, which effectively stops translating the
	 * Request TLPs across the boundary of local partition */
	pci_clear_master(pdata->pdev);

	/* Disable Completion TLPs */
	idt_ntb_writefld_mem(cfg, IDT_NT_CPEN, OFF);

	/* Disable the corresponding NT Mapping table entry */
	idt_ntb_writereg(cfg, IDT_NT_PCI_NTMTBLADDR, (u32)pdata->part);
	idt_ntb_writereg(cfg, IDT_NT_PCI_NTMTBLDATA, (u32)OFF);

	/* The ndevs->lnk_sts variable is going to change in the work thread */
}

/*
 * Notify the peer device that the local side is ready.
 *
 * Since the Primary side can't enable/disable link by demand of the client
 * driver, there should be some way to notify the opposite side, what the local
 * client driver is installed and started working (by calling the
 * ntb_enable_link method). So Global Signal register is used for that purpose.
 */
static void idt_ntb_link_notify(struct idt_ntb_dev *ndev)
{
	void __iomem *cfg = to_cfg_ndev(ndev);

	/* Just write ON to the first bit of device NTGSIGNAL register
	 * It is available only using GASA* registers */
	idt_ntb_writereg(cfg, portdata_tbl[ndev->port].ntgsignal, ON);
}

/*
 * Clear the notification set before in the Global Signal Status register.
 */
static void idt_ntb_link_clear_notification(struct idt_ntb_dev *ndev)
{
	void __iomem *cfg = to_cfg_ndev(ndev);

	/* Clear the Global Signal status bit of the device partition */
	idt_ntb_writereg(cfg, IDT_SW_PCI_SEGSIGSTS, ((u32)1 << ndev->part));
}

/*
 * Retrieve the current link status
 */
static int idt_ntb_link_status(struct idt_ntb_dev *ndev)
{
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	void __iomem *cfg = to_cfg_ndev(ndev);
	u32 localbme, peerbme, pciests, gsigsts;
	unsigned int part;

	/* Read the local Bus Master Enable status */
	localbme = idt_ntb_readfld_mem(cfg, IDT_NT_BME);

	/* Read the Global Signal Status bit related to the device partition */
	gsigsts = idt_ntb_readreg(cfg, IDT_SW_PCI_SEGSIGSTS);
	/* Retrieve the partition of the corresponding device */
	part = (NTB_TOPO_PRI == pdata->role) ? ndev->part : pdata->part;
	gsigsts = (gsigsts & ((u32)1 << part)) ? ON : OFF;

	/* Read the peer Bus Master Enable status */
	peerbme = idt_ntb_readreg(cfg, portdata_tbl[ndev->port].pcicmd);
	peerbme = idt_ntb_readfld_var(peerbme, IDT_NT_BME);

	/* Retrieve the peer port link status */
	pciests = idt_ntb_readreg(cfg, portdata_tbl[ndev->port].sts);
	pciests = idt_ntb_readfld_var(pciests, IDT_SW_PORT_LNKUP);

	/* If Both BME fields are ON and PCIe data link is up then the NTB
	 * link is effectively up */
	if (ON == pciests && ON == peerbme && ON == localbme && ON == gsigsts) {
		return ON;
	} /* else if (OFF == pciests || OFF == peerbme || Off == localbme ||
	   *          OFF == gsigsts) {return OFF} */

	return OFF;
}

/*
 * Kernel thread polling the peer side link status by reading the corresponding
 * PCIe link status register and NT Mapping table entry
 */
static void idt_ntb_poll_link_work(struct work_struct *work)
{
	struct idt_ntb_data *pdata = to_data_lnkwork(work);
	struct idt_ntb_dev *ndev;
	unsigned char id;
	int curlnksts;

	/* Walk through all available peers reading their status */
	for (id = 0; id < pdata->peer_cnt; id++) {
		/* Get the current NTB device */
		ndev = &pdata->ndevs[id];

		/* Retrieve the current link status */
		curlnksts = idt_ntb_link_status(ndev);

		/* If the link status has changed then call the event handler */
		if (curlnksts != ndev->lnk_sts) {
			ndev->lnk_sts = curlnksts;
			ntb_link_event(&ndev->ntb);
		}
	}

	/* Reschedule the work */
	(void)queue_delayed_work(pdata->idt_wq, &pdata->lnk_work,
				 IDT_NTB_LNKPOLL_TOUT);
}

/*
 * Initialize NTB link subsystem
 *
 * NOTE This function is not used by the client driver but just for
 *      initialization
 */
static void idt_ntb_init_link(struct idt_ntb_data *pdata)
{
	unsigned char id;

	/* Initialize all the peers link status with OFF */
	for (id = 0; id < pdata->peer_cnt; id++) {
		pdata->ndevs[id].lnk_sts = OFF;
	}

	/* Enable the link if it's primary port */
	if (NTB_TOPO_PRI == pdata->role) {
		/* Clear all the Global Signal Status bits related to the
		 * locally available NTB device */
		for (id = 0; id < pdata->peer_cnt; id++) {
			idt_ntb_link_clear_notification(&pdata->ndevs[id]);
		}
		/* Next function enables the whole link no matter which NTB
		 * device it's */
		idt_ntb_link_effective_enable(pdata);
	}

	/* Initialize the delayed kernel thread polling the link status */
	INIT_DELAYED_WORK(&pdata->lnk_work, idt_ntb_poll_link_work);
	(void)queue_delayed_work(pdata->idt_wq, &pdata->lnk_work,
				 IDT_NTB_LNKPOLL_TOUT);

	dev_dbg_data(pdata, "IDT NTB peer device link polling started");
}

/*
 * Clear the link polling subsystem
 *
 * NOTE This function is not used by the client driver but just for
 *      final deinitialization
 */
static void idt_ntb_clear_link(struct idt_ntb_data *pdata)
{
	unsigned char id;

	/* Stop the link status polling thread */
	cancel_delayed_work_sync(&pdata->lnk_work);

	/* Disable the link */
	idt_ntb_link_effective_disable(pdata);

	/* Clear all the Global Signal Status bits related to the
	 * Primary port available NTB device */
	if (NTB_TOPO_PRI == pdata->role) {
		for (id = 0; id < pdata->peer_cnt; id++) {
			idt_ntb_link_clear_notification(&pdata->ndevs[id]);
		}
	}

	dev_dbg_data(pdata, "IDT NTB peer device link polling stopped");
}

/*
 * NTB bus callback - get the current ntb link state
 */
static int idt_ntb_link_is_up(struct ntb_dev *ntb, enum ntb_speed *speed,
			      enum ntb_width *width)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	void __iomem *cfg = to_cfg_ndev(ndev);
	u32 pcielsts;
	int lnksts;

	/* Get the curret link status */
	lnksts = idt_ntb_link_status(ndev);

	/* Retrieve the PCIe data link parameters */
	if (ON == lnksts) {
		/* Read the PCIe link status */
		pcielsts = idt_ntb_readreg(cfg,
			portdata_tbl[ndev->port].pcielsts);
		/* The register values numerically match the enum values */
		if (speed) {
			*speed = idt_ntb_readfld_var(pcielsts, IDT_NT_CURLNKSPD);
		}
		if (width) {
			*width = idt_ntb_readfld_var(pcielsts, IDT_NT_CURLNKWDTH);
		}
	} else /* if (OFF == lnksts) */ {
		if (speed) {
			*speed = NTB_SPEED_NONE;
		}
		if (width) {
			*width = NTB_WIDTH_NONE;
		}
	}

	return lnksts;
}

/*
 * NTB bus callback - enable the link on the secondary side of the ntb
 *
 * NOTE Since there can be more than one pair of NTB devices (we use shared
 * Lookup table) on the Primary port, the link must be always enabled from that
 * side. So the next function fully works from the Secondary side only.
 */
static int idt_ntb_link_enable(struct ntb_dev *ntb, enum ntb_speed speed,
			       enum ntb_width width)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	void __iomem *cfg = pdata->cfg_mmio;

	/* Primary port driver enables the link in the initialization method */
	if (NTB_TOPO_PRI == ntb->topo) {
		/* Notify the opposite side, that the link is enabled */
		idt_ntb_link_notify(ndev);

		dev_dbg_ndev(ndev, "IDT NT-function link is virtually enabled");

		return -EINVAL;
	}

	/* Secondary ports can effectively enable the link on the local side */
	idt_ntb_link_effective_enable(pdata);

	/* Enable the interrupts of message, doorbells, switch and temperature
	 * sensor events. This will generate all the pending interrupts after the
	 * link is effectively enabled */
	idt_ntb_writereg(cfg, IDT_NT_PCI_NTINTMSK, NTINT_UNMASK);

	dev_dbg_ndev(ndev, "IDT NT-function link is enabled");

	return SUCCESS;
}

/*
 * NTB bus callback - disable the link on the secondary side of the ntb
 */
static int idt_ntb_link_disable(struct ntb_dev *ntb)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	void __iomem *cfg = pdata->cfg_mmio;

	/* Primary port driver disables the link in the link clear method */
	if (NTB_TOPO_PRI == ntb->topo) {
		/* Notify the opposite side, that the link is disabled */
		idt_ntb_link_clear_notification(ndev);

		dev_dbg_ndev(ndev, "IDT NT-function link is virtually disabled");

		return -EINVAL;
	}

	/* Disable the interrupts of message, doorbells, switch and temperature
	 * sensor events. This will stop generateing interrupts while link is
	 * down */
	idt_ntb_writereg(cfg, IDT_NT_PCI_NTINTMSK, NTINT_MASK);

	/* Secondary ports can effectively disable the link on the local side */
	idt_ntb_link_effective_disable(pdata);

	dev_dbg_ndev(ndev, "IDT NT-function link is disabled");

	return SUCCESS;
}

/*===========================================================================
 *                         4. Memory Window subsystem
 *===========================================================================*/

/*
 * Find the Secondary port serial number (id) by the passed primary and
 * secondary ports
 */
static inline unsigned char idt_ntb_findid(struct idt_ntb_topo *topo,
					   unsigned char pri, unsigned char sec)
{
	return hweight32(topo->secports[pri] & (((u32)1 << sec) - 1));
}

/*
 * Initialize the PCI device BAR2(3:x64) setup register
 */
static int idt_ntb_setup_bar2(struct idt_ntb_data *pdata)
{
	void __iomem *cfg = pdata->cfg_mmio;
	phys_addr_t limit;
	int ret;

	/* Request the PCI resources for the BAR2(3) */
	ret = pci_request_region(pdata->pdev, BAR2, NTB_NAME);
	if (SUCCESS != ret) {
		dev_err_data(pdata,
			"Failed to request the PCI BAR2(3) resources");
		return ret;
	}

	/* Retrieve the physical address of the mapped by the Lookup table
	 * shared memory - BAR2(3) */
	pdata->mw_base = pci_resource_start(pdata->pdev, BAR2);

	/* Limit the BAR2 address with resepect to the Lookup table boundary */
	/* Calculate the size of just one Memory Window */
	pdata->mw_size = pci_resource_len(pdata->pdev, BAR2)/32;

	/* Find the limit address */
	limit = pdata->mw_base + IDT_NTB_MW_CNT * pdata->mw_size - 1;

	/* Set the BAR size limiting register */
	idt_ntb_writereg(cfg, IDT_NT_PCI_BARLIMIT2, (u32)limit);
#ifdef CONFIG_64BIT
	idt_ntb_writereg(cfg, IDT_NT_PCI_BARLIMIT3, (u32)(limit >> 32));
#endif /* CONFIG_64BIT */

	return SUCCESS;
}

/*
 * Deinitialize the PCI device BAR2(3:x64) setup register
 */
static void idt_ntb_clean_bar2(struct idt_ntb_data *pdata)
{
	void __iomem *cfg = pdata->cfg_mmio;
	u32 limit = -1;

	/* Set the BAR size limiting register */
	idt_ntb_writereg(cfg, IDT_NT_PCI_BARLIMIT2, limit);
#ifdef CONFIG_64BIT
	idt_ntb_writereg(cfg, IDT_NT_PCI_BARLIMIT3, limit);
#endif /* CONFIG_64BIT */

	/* Just write the disabled BARSETUP0 */
	pci_release_region(pdata->pdev, BAR2);
}

/*
 * Set the Memory Window translation address for the passed peer NTB device
 */
static int idt_ntb_setmw(struct idt_ntb_dev *ndev, const int mwindx,
			 const dma_addr_t addr)
{
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	void __iomem *cfg = to_cfg_ndev(ndev);
	u32 lut_indxbar = 0, lut_partval = 0;
	unsigned long irqflags;

	/* Return error if the passed memory window index is out of range */
	if (mwindx >= ndev->mw_self_cnt) {
		dev_err_ndev(ndev,
			"Invalid Memory Window index specified to set");
		return -EINVAL;
	}

	/* Return error if the passed address is not aligned with the four
	 * bytes */
	if (!IS_ALIGNED(addr, IDT_NTB_TRANSALIGN)) {
		dev_err_ndev(ndev, "Translated base address is not aligned");
		return -EINVAL;
	}

	/* Collect the Lookup table offset */
	idt_ntb_writefld_var(&lut_indxbar, IDT_NT_LUT_INDEX,
			     ndev->mw_self_offset + mwindx);
	idt_ntb_writefld_var(&lut_indxbar, IDT_NT_LUT_BAR, BAR2);

	/* Collect the Lookup table entry partition and valid bits */
	idt_ntb_writefld_var(&lut_partval, IDT_NT_LUT_PART, ndev->part);
	idt_ntb_writefld_var(&lut_partval, IDT_NT_LUT_VALID, ON);

	/* Start critical section writing to the local port Lookup table */
	spin_lock_irqsave(&pdata->lut_lock, irqflags);
	/* Write the data to the Lookup table registers of the peer */
	idt_ntb_writereg(cfg, IDT_NT_PCI_LUTOFFSET, lut_indxbar);
	idt_ntb_writereg(cfg, IDT_NT_PCI_LUTLDATA, (u32)addr);
#ifdef CONFIG_64BIT
	idt_ntb_writereg(cfg, IDT_NT_PCI_LUTMDATA, (u32)(addr >> 32));
#else
	idt_ntb_writereg(cfg, IDT_NT_PCI_LUTMDATA, (u32)0);
#endif /* !CONFIG_64BIT */
	idt_ntb_writereg(cfg, IDT_NT_PCI_LUTUDATA, lut_partval);
	/* Finally unlock the Lookup table */
	spin_unlock_irqrestore(&pdata->lut_lock, irqflags);

	return SUCCESS;
}

/*
 * Set the Memory Window translation address for the passed peer NTB device
 */
static int idt_ntb_unsetmw(struct idt_ntb_dev *ndev, const int mwindx)
{
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	void __iomem *cfg = to_cfg_ndev(ndev);
	u32 lut_indxbar = 0, lut_partval = 0;
	unsigned long irqflags;

	/* Return Error if the passed Memory Window index is out of range */
	if (mwindx >= ndev->mw_self_cnt) {
		dev_err_ndev(ndev,
			"Invalid Memory Window index specified to unset");
		return -EINVAL;
	}

	/* Collect the Lookup table offset */
	idt_ntb_writefld_var(&lut_indxbar, IDT_NT_LUT_INDEX,
			     ndev->mw_self_offset + mwindx);
	idt_ntb_writefld_var(&lut_indxbar, IDT_NT_LUT_BAR, BAR2);

	/* Collect the Lookup table entry partition and valid bits */
	idt_ntb_writefld_var(&lut_partval, IDT_NT_LUT_VALID, OFF);

	/* Start critical section writing to the Lookup table */
	spin_lock_irqsave(&pdata->lut_lock, irqflags);
	/* Write the data to the Lookup table registers of the peer */
	idt_ntb_writereg(cfg, IDT_NT_PCI_LUTOFFSET, lut_indxbar);
	idt_ntb_writereg(cfg, IDT_NT_PCI_LUTLDATA, (u32)0);
	idt_ntb_writereg(cfg, IDT_NT_PCI_LUTMDATA, (u32)0);
	idt_ntb_writereg(cfg, IDT_NT_PCI_LUTUDATA, lut_partval);
	/* Finally unlock the Lookup table */
	spin_unlock_irqrestore(&pdata->lut_lock, irqflags);

	return SUCCESS;
}

/*
 * Cleanup the local Lookup table
 */
static int idt_ntb_cleanlut(struct idt_ntb_data *pdata)
{
	struct idt_ntb_dev *ndev;
	unsigned char id, mw;
	int ret;

	/* Walk through all the available peers */
	for (id = 0; id < pdata->peer_cnt; id++) {
		ndev = &pdata->ndevs[id];

		/* Unset all the local memory windows */
		for (mw = 0; mw < ndev->mw_self_cnt; mw++) {
			ret = idt_ntb_unsetmw(ndev, mw);
			if (SUCCESS != ret) {
				return ret;
			}
		}
	}

	return SUCCESS;
}

/*
 * Initialize the Memory Windows for the current NT-function with respect to the
 * topologically predefined NTB pairs
 *
 * NOTE The first NTB pairs are lucky to have the extended set of Memory Windows
 */
static int idt_ntb_init_mws(struct idt_ntb_data *pdata)
{
	struct idt_ntb_topo *topo = &pdata->topo;
	struct idt_ntb_dev *ndevs = pdata->ndevs;
	unsigned char id, mwcnt, luckies, curoffset;
	int ret;

	/* Calculate the number of Memory Windows per NTB */
	mwcnt = IDT_NTB_MW_CNT / topo->paircnt;
	luckies = IDT_NTB_MW_CNT % topo->paircnt;

	/* Find the memory windows local and peer parameters */
	if (NTB_TOPO_PRI == pdata->role) {
		/* Loop over all the locally available peers */
		curoffset = 0;
		for (id = 0; id < pdata->peer_cnt; id++) {
			/* Find the memory windows offset and count */
			ndevs[id].mw_self_offset = curoffset;
			ndevs[id].mw_self_cnt = mwcnt + (luckies > id ? 1 : 0);
			ndevs[id].mw_peer_cnt = IDT_NTB_MW_CNT;

			/* Get the offset for the next Memory Windows */
			curoffset += ndevs[id].mw_self_cnt;
		}
	} else /* if (NTB_TOPO_SEC == pdata->role) */ {
		id = ndevs[0].pairid;
		ndevs[0].mw_self_offset = 0;
		ndevs[0].mw_self_cnt = IDT_NTB_MW_CNT;
		ndevs[0].mw_peer_cnt = mwcnt + (luckies > id ? 1 : 0);
	}

	/* Initialize the BAR2(3) related registers and data fields */
	ret = idt_ntb_setup_bar2(pdata);
	if (SUCCESS != ret) {
		return ret;
	}

	/* Initialize the Lookup table spinlock*/
	spin_lock_init(&pdata->lut_lock);

	/* Cleanup the Lookup table */
	(void)idt_ntb_cleanlut(pdata);

	dev_dbg_data(pdata, "IDT NTB device memory windows redistributed");

	return SUCCESS;
}

/*
 * Clean the Memory Windows initialized for the current NT-function
 */
static void idt_ntb_clean_mws(struct idt_ntb_data *pdata)
{
	/* Cleanup the peers Lookup tables */
	(void)idt_ntb_cleanlut(pdata);

	/* Clean the BAR2(3) */
	idt_ntb_clean_bar2(pdata);

	dev_dbg_data(pdata, "IDT NTB function memory windows cleaned");
}

/*
 * NTB bus callback - local memory windows count
 */
static int idt_ntb_mw_count(struct ntb_dev *ntb)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);

	/* Return the number of available local memory windows */
	return ndev->mw_self_cnt;
}

/*
 * NTB bus callback - get the map resource of a memory window
 */
static int idt_ntb_mw_get_maprsc(struct ntb_dev *ntb, int idx, phys_addr_t *base,
				 resource_size_t *size)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	struct idt_ntb_data *pdata = to_data_ndev(ndev);

	/* It's error to pass the out of range Memory Window index */
	if (idx >= ndev->mw_self_cnt) {
		dev_err_ndev(ndev,
			"Invalid memory window index passed to get map res");
		return -EINVAL;
	}

	/* The base address is determined with respect to the Lookup table
	 * table offset */
	if (base)
		*base = pdata->mw_base +
			(ndev->mw_self_offset + idx) * pdata->mw_size;
	if (size)
		*size = pdata->mw_size;

	return SUCCESS;
}

/*
 * NTB bus callback - get the local memory windows alignments
 */
static int idt_ntb_mw_get_align(struct ntb_dev *ntb, int idx,
				resource_size_t *addr_align,
				resource_size_t *size_align,
				resource_size_t *size_max)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	struct idt_ntb_data *pdata = to_data_ndev(ndev);

	/* It's error to pass the out of range Memory Window index */
	if (idx >= ndev->mw_self_cnt) {
		dev_err_ndev(ndev,
			"Invalid memory window index passed to get alignment");
		return -EINVAL;
	}

	/* According to standard the address should be alignment within 4KB */
	if (addr_align)
		*addr_align = SZ_4K;
	/* Size alignment and max size effectively make the size fixed to
	 * size_max */
	if (size_align)
		*size_align = pdata->mw_size;
	if (size_max)
		*size_max = pdata->mw_size;

	return SUCCESS;
}

/*
 * NTB bus callback - set the translation of a Memory Window
 */
static int idt_ntb_mw_set_trans(struct ntb_dev *ntb, int idx, dma_addr_t addr,
				resource_size_t size)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	int ret;

	/* Although the passed size is not used anywhere, we need to make sure
	 * the size fits the memory window */
	if (0 != size && size != pdata->mw_size) {
		dev_err_ndev(ndev,
			"Invalid translated address size was specified");
		return -EINVAL;
	}

	/* Set the passed memory window or unset it if the size is zero */
	if (0 != size) {
		ret = idt_ntb_setmw(ndev, idx, addr);
	} else /* if (0 == size) */ {
		ret = idt_ntb_unsetmw(ndev, idx);
	}

	return ret;
}

/*
 * NTB bus callback - peer memory windows count
 */
static int idt_ntb_peer_mw_count(struct ntb_dev *ntb)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);

	/* Return the number of available peer memory windows */
	return ndev->mw_peer_cnt;
}

/*
 * NTB bus callback - get the peer memory windows alignments
 */
static int idt_ntb_peer_mw_get_align(struct ntb_dev *ntb, int idx,
				     resource_size_t *addr_align,
				     resource_size_t *size_align,
				     resource_size_t *size_max)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	struct idt_ntb_data *pdata = to_data_ndev(ndev);

	/* It's error to pass the out of range Memory Window index */
	if (idx >= ndev->mw_peer_cnt) {
		dev_err_ndev(ndev,
			"Invalid memory window index passed to get "
			"peer alignment");
		return -EINVAL;
	}

	/* Although there are only two unmodifiable LS-bits in lookup table
	 * entries, according to standard the address should be aligned
	 * within 4KB */
	if (addr_align)
		*addr_align = SZ_4K;
	/* Size alignment and max size effectively make the size fixed to
	 * size_max */
	if (size_align)
		*size_align = pdata->mw_size;
	if (size_max)
		*size_max = pdata->mw_size;

	return SUCCESS;
}

/*===========================================================================
 *                          5. Doorbells subsystem
 *===========================================================================*/

static void idt_ntb_db_tasklet(unsigned long data);

/*
 * Initialize the Global Doorbell Mask
 *
 * NOTE Initialize the Inbound Doorbell mask so the local event can
 *      be rised by the self Doorbells bits only. The Outbound
 *      Doorbell is setup so the local port could set both self
 *      and peer Doorbells. Due to the self and peer masks swap
 *      the following loops should work well on the both sides
 */
static void idt_ntb_init_gdbellmsk(struct idt_ntb_data *pdata, unsigned char id)
{
	void __iomem *cfg = pdata->cfg_mmio;
	struct idt_ntb_dev *ndevs = pdata->ndevs;
	u32 selfpartbits, peerpartbits;
	int setbit;

	/* There is a bug if the passed id exceeds the total number of peers */
	BUG_ON(id >= pdata->peer_cnt);

	/* Get the self and peer partition masks */
	selfpartbits = ~((u32)1 << pdata->part);
	peerpartbits = ~((u32)1 << ndevs[id].part);

	/* Init the self Doorbell masks */
	for_each_set_bit_u32(ndevs[id].db_self_mask, setbit) {
		idt_ntb_writereg(cfg, IDT_SW_PCI_GIDBELLMSK0 + setbit,
				 selfpartbits);
		idt_ntb_writereg(cfg, IDT_SW_PCI_GODBELLMSK0 + setbit,
				 selfpartbits & peerpartbits);
	}

	/* Init the peer Doorbell masks */
	for_each_set_bit_u32(ndevs[id].db_peer_mask, setbit) {
		idt_ntb_writereg(cfg, IDT_SW_PCI_GIDBELLMSK0 + setbit,
				 peerpartbits);
		idt_ntb_writereg(cfg, IDT_SW_PCI_GODBELLMSK0 + setbit,
				 selfpartbits & peerpartbits);
	}
}

/*
 * Deinitialize the Global Doorbell Mask
 *
 * Function is unused to make sure the NTB devices can be unloaded without
 * any serious consequences for the peer device.
 */
static void __maybe_unused idt_ntb_clean_gdbellmsk(struct idt_ntb_data *pdata,
						   unsigned char id)
{
	void __iomem *cfg = pdata->cfg_mmio;
	struct idt_ntb_dev *ndevs = pdata->ndevs;
	int setbit;

	/* There is a bug if the passed id exceeds the total number of peers */
	BUG_ON(id >= pdata->peer_cnt);

	/* Deinit the self Doorbell masks */
	for_each_set_bit_u32(ndevs[id].db_self_mask, setbit) {
		idt_ntb_writereg(cfg, IDT_SW_PCI_GIDBELLMSK0 + setbit,
				 (u32)0);
		idt_ntb_writereg(cfg, IDT_SW_PCI_GODBELLMSK0 + setbit,
				 (u32)0);
	}
	/* Deinit the peer Doorbell masks */
	for_each_set_bit_u32(ndevs[id].db_peer_mask, setbit) {
		idt_ntb_writereg(cfg, IDT_SW_PCI_GIDBELLMSK0 + setbit,
				 (u32)0);
		idt_ntb_writereg(cfg, IDT_SW_PCI_GODBELLMSK0 + setbit,
				 (u32)0);
	}
}

/*
 * Initialize the Doorbells for the current NT-function with respect to the
 * topologically predefined NTB pairs
 *
 * NOTE The first NTB pairs are lucky to have the extended set of Doorbells
 */
static void idt_ntb_init_db(struct idt_ntb_data *pdata)
{
	struct idt_ntb_topo *topo = &pdata->topo;
	struct idt_ntb_dev *ndevs = pdata->ndevs;
	unsigned char id, dbcntstd, dbcntext, dbleft, luckies, pairid, dboffset;
	u32 pridbmask, secdbmask;

	/* Calculate the number of Doorbells per pair and the leftovers */
	dbcntstd = IDT_NTB_DBELL_CNT / topo->paircnt;
	dbleft = IDT_NTB_DBELL_CNT % topo->paircnt + (dbcntstd % 2) * topo->paircnt;
	/* Alter the db count to be even */
	dbcntstd = (dbcntstd / 2) * 2;
	dbcntext = dbcntstd + 2;

	/* Number of the lucky pairs having additional Doorbells */
	luckies = dbleft / 2;

	/* Loop over all the locally available peers */
	for (id = 0; id < pdata->peer_cnt; id++) {
		/* Current pair ID */
		pairid = ndevs[id].pairid;

		/* Retrieve the doorbells count and the doorbells offset for the
		 * current pair ID (the first luckies have extended doorbells) */
		if (luckies > pairid) {
			ndevs[id].db_cnt = dbcntext / 2;
			dboffset = dbcntext * pairid;
		} else {
			ndevs[id].db_cnt = dbcntstd / 2;
			dboffset = dbcntext * luckies +
				   dbcntstd * (pairid - luckies);
		}

		/* Calculate the valid Doorbells mask for the corresponding
		 * ports */
		ndevs[id].db_valid_mask = ((u32)1 << ndevs[id].db_cnt) - 1;
		pridbmask = ndevs[id].db_valid_mask << dboffset;
		secdbmask = pridbmask << ndevs[id].db_cnt;

		/* Initialize the corresponding Device structure fields */
		if (NTB_TOPO_PRI == pdata->role) {
			ndevs[id].db_self_mask = pridbmask;
			ndevs[id].db_self_offset = dboffset;
			ndevs[id].db_peer_mask = secdbmask;
			ndevs[id].db_peer_offset = dboffset + ndevs[id].db_cnt;
		} else /* if (NTB_TOPO_SEC == pdata->role) */ {
			ndevs[id].db_self_mask = secdbmask;
			ndevs[id].db_self_offset = dboffset + ndevs[id].db_cnt;
			ndevs[id].db_peer_mask = pridbmask;
			ndevs[id].db_peer_offset = dboffset;
		}

		/* Initialize the corresponding Global Doorbell masks. It can be
		 * done by both Primary and Secondary ports */
		idt_ntb_init_gdbellmsk(pdata, id);
	}

	/* Initialize the spin lock to sync access to the self doorbell status
	 * and mask variables */
	pdata->db_sts = 0;
	pdata->db_msk = (u32)-1;
	/* In fact db_lock is used at most at tasklet so BH lock would be enough,
	 * but the critical section can be accessed in the db event handler,
	 * which is protected by the context irqsave spin lock. So calling BH
	 * spin locker/unlocker function would cause the OOPS Warning of
	 * local_bh_enable_ip method. Therefore the irqsave/irqrestore methods
	 * are used to synchronize access to the db_sts and db_msk fields*/
	spin_lock_init(&pdata->db_lock);

	/* Initialize the doorbells tasklet */
	tasklet_init(&pdata->db_tasklet, idt_ntb_db_tasklet,
		     (unsigned long)pdata);

	/* Unmask the inbound doorbell interrupts */
	idt_ntb_writereg(pdata->cfg_mmio, IDT_NT_PCI_INDBELLMSK, INDB_UNMASK);

	dev_dbg_data(pdata, "IDT NTB device doorbells initialized");
}

/*
 * Clean the Doorbells initialized for the pairs of NT-functions
 *
 * It just makes all the NT-functions being able to use the self and peer
 * Doorbells
 */
static void idt_ntb_clean_db(struct idt_ntb_data *pdata)
{
	/*unsigned char id;*/

	/* Just kill the tasklet */
	tasklet_kill(&pdata->db_tasklet);

	/* Just clean the Doorbell masks for all the peers as they must have
	 * initially been. Do it by the Primary side only */
	/*if (NTB_TOPO_PRI == pdata->role) {
		for (id = 0; id < pdata->peer_cnt; id++) {
			idt_ntb_clean_gdbellmsk(pdata, id);
		}
	}*/

	dev_dbg_data(pdata, "IDT NTB device doorbells deinitilized");
}

/*
 * Doorbells event tasklet
 */
static void idt_ntb_db_tasklet(unsigned long data)
{
	struct idt_ntb_data *pdata = (struct idt_ntb_data *)data;
	struct idt_ntb_dev *ndevs = pdata->ndevs;
	void __iomem *cfg = pdata->cfg_mmio;
	u32 db_sts, db_self, db_sts_prev;
	unsigned long setbit, irqflags;
	unsigned char id;

	/* NOTE All doorbells are masked to generate the interrupt by the IRQ
	 *      handler until the cause of the interrupt is handled */
	db_sts = idt_ntb_readreg(cfg, IDT_NT_PCI_INDBELLSTS);
	/* Clear all the retrieved doorbell bits */
	idt_ntb_writereg(cfg, IDT_NT_PCI_INDBELLSTS, db_sts);
	/* Finally unmask the doorbells interrupt. The next action shall rise
	 * the interrupt if any doorbell bit was set after the register had
	 * been read and cleared */
	idt_ntb_writereg(cfg, IDT_NT_PCI_INDBELLMSK, INDB_UNMASK);

	/** START Sync access to the doorbell variables */
	spin_lock_irqsave(&pdata->db_lock, irqflags);
	/* Retrieve the current doorbell status bits */
	db_sts_prev = pdata->db_sts;
	/* Set the new doorbell status */
	pdata->db_sts |= db_sts;
	/* There are going to be handled only the doorbell bits, which have not
	 * been set before and also have not been masked */
	db_sts &= ~db_sts_prev & ~pdata->db_msk;
	/** END The critical section of access to the doorbell variables */
	spin_unlock_irqrestore(&pdata->db_lock, irqflags);

	/* If the new doorbell status bits are masked then do nothing */
	if (!db_sts) {
		dev_dbg_data(pdata, "Got masked doorbell interrupt");
		return;
	}

	/* Walk through all the peers looking for the relevant one to handle
	 * new doorbells */
	for (id = 0; id < pdata->peer_cnt; id++) {
		/* Invoke the context callback if there are doorbells set for
		 * the current NTB device */
		db_self = (db_sts & ndevs[id].db_self_mask);
		db_self >>= ndevs[id].db_self_offset;
		for_each_set_bit_u32(db_self, setbit) {
			ntb_db_event(&ndevs[id].ntb, (int)setbit);
		}
	}
}

/*
 * NTB bus callback - get a mask of doorbell bits supported by the ntb
 */
static u64 idt_ntb_db_valid_mask(struct ntb_dev *ntb)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);

	/* Return the valid doorbell bits mask */
	return ndev->db_valid_mask;
}

/*
 * NTB bus callback - get the number of doorbell interrupt vectors
 */
static int idt_ntb_db_vector_count(struct ntb_dev *ntb)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);

	/* Number of doorbell vectors equal to the doorbell bits count */
	return ndev->db_cnt;
}

/*
 * NTB bus callback - get a mask of doorbell bits serviced by a vector
 */
static u64 idt_ntb_db_vector_mask(struct ntb_dev *ntb, int db_vec)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);

	if (db_vec < 0 || ndev->db_cnt <= db_vec) {
		return 0;
	}

	/* Each doorbell bit corresponds to the vector so the mask is just one
	 * shifted bit */
	return ((u64)1 << db_vec);
}

/*
 * NTB bus callback - read the local doorbell register
 */
static u64 idt_ntb_db_read(struct ntb_dev *ntb)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	unsigned long irqflags;
	u32 db_sts;

	/** START Sync access to the doorbell variables */
	spin_lock_irqsave(&pdata->db_lock, irqflags);
	/* Read the current doorbell status */
	db_sts = pdata->db_sts;
	/** END The critical section of access to the doorbell variables */
	spin_unlock_irqrestore(&pdata->db_lock, irqflags);

	/* Return the accordingly shifted doorbell bits */
	return (db_sts & ndev->db_self_mask) >> ndev->db_self_offset;
}

/*
 * NTB bus callback - set bits in the local doorbell register
 *
 * NOTE It must be done using the doorbell register io to generate the
 *      interrupt and invoke the doorbell event handler set by the client
 *      driver
 */
static int idt_ntb_db_set(struct ntb_dev *ntb, u64 db_bits)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	void __iomem *cfg = to_cfg_ndev(ndev);

	/* Return error if invalid bits are set */
	if (db_bits & ~ndev->db_valid_mask) {
		dev_dbg_ndev(ndev,
			"Invalid doorbell bits are passed to locally set");
		return -EINVAL;
	}

	/* Set the corresponding bits in the doorbell register */
	idt_ntb_writereg(cfg, IDT_NT_PCI_OUTDBELLSET,
			 ((u32)db_bits << ndev->db_self_offset));

	return SUCCESS;
}

/*
 * NTB bus callback - clear bits in the local doorbell register
 */
static int idt_ntb_db_clear(struct ntb_dev *ntb, u64 db_bits)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	unsigned long irqflags;

	/* Return error if invalid bits are set */
	if (db_bits & ~ndev->db_valid_mask) {
		dev_dbg_ndev(ndev,
			"Invalid doorbell bits are passed to locally clear");
		return -EINVAL;
	}

	/** START Sync access to the doorbell variables */
	spin_lock_irqsave(&pdata->db_lock, irqflags);
	/* Read the current doorbell status */
	pdata->db_sts &= ~((u32)db_bits << ndev->db_self_offset);
	/** END The critical section of access to the doorbell variables */
	spin_unlock_irqrestore(&pdata->db_lock, irqflags);

	return SUCCESS;
}

/*
 * NTB bus callback - read the local doorbell mask
 */
static u64 idt_ntb_db_read_mask(struct ntb_dev *ntb)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	unsigned long irqflags;
	u32 db_msk;

	/** START Sync access to the doorbell variables */
	spin_lock_irqsave(&pdata->db_lock, irqflags);
	/* Read the current doorbell mask */
	db_msk = pdata->db_msk;
	/** END The critical section of access to the doorbell variables */
	spin_unlock_irqrestore(&pdata->db_lock, irqflags);

	/* Return the accordingly shifted doorbell bits */
	return (db_msk & ndev->db_self_mask) >> ndev->db_self_offset;
}

/*
 * NTB bus callback - set bits in the local doorbell mask
 */
static int idt_ntb_db_set_mask(struct ntb_dev *ntb, u64 db_bits)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	unsigned long irqflags;

	/* Return error if invalid bits are set */
	if (db_bits & ~ndev->db_valid_mask) {
		dev_dbg_ndev(ndev,
			"Invalid field is passed to set the doorbell mask");
		return -EINVAL;
	}

	/** START Sync access to the doorbell variables */
	spin_lock_irqsave(&pdata->db_lock, irqflags);
	/* Set the corresponding bits in the local mask */
	pdata->db_msk |= ((u32)db_bits << ndev->db_self_offset);
	/** END The critical section of access to the doorbell variables */
	spin_unlock_irqrestore(&pdata->db_lock, irqflags);

	return SUCCESS;
}

/*
 * NTB bus callback - clear bits in the local doorbell mask
 */
static int idt_ntb_db_clear_mask(struct ntb_dev *ntb, u64 db_bits)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	u32 db_sts, unmask_bits;
	unsigned long setbit, irqflags;

	/* Return error if invalid bits are set */
	if (db_bits & ~ndev->db_valid_mask) {
		dev_dbg_ndev(ndev,
			"Invalid field is passed to clear the doorbell mask");
		return -EINVAL;
	}

	/* Calculate the unmaskable bits first */
	unmask_bits = ((u32)db_bits << ndev->db_self_offset);

	/** START Sync access to the doorbell variables */
	spin_lock_irqsave(&pdata->db_lock, irqflags);
	/* Retrieve the doorbell status bits, which have been masked, but are
	 * going to be unmasked now */
	db_sts = pdata->db_sts & pdata->db_msk & unmask_bits;
	/* Clear the corresponding bits in the local mask */
	pdata->db_msk &= ~unmask_bits;
	/** END The critical section of access to the doorbell variables */
	spin_unlock_irqrestore(&pdata->db_lock, irqflags);

	/* Invoke the context callback if there are set doorbells, which have
	 * just been unmasked */
	db_sts = (db_sts & ndev->db_self_mask) >> ndev->db_self_offset;
	for_each_set_bit_u32(db_sts, setbit) {
		ntb_db_event(&ndev->ntb, (int)setbit);
	}

	return SUCCESS;
}

/*
 * NTB bus callback - set bits in the peer doorbell register
 */
static int idt_ntb_peer_db_set(struct ntb_dev *ntb, u64 db_bits)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	void __iomem *cfg = to_cfg_ndev(ndev);

	/* Return error if invalid bits are set */
	if (db_bits & ~ndev->db_valid_mask) {
		dev_dbg_ndev(ndev,
			"Invalid doorbell bits are passed to remotely set");
		return -EINVAL;
	}

	/* Set the corresponding bits in the doorbell register */
	idt_ntb_writereg(cfg, IDT_NT_PCI_OUTDBELLSET,
			 ((u32)db_bits << ndev->db_peer_offset));

	return SUCCESS;
}

/*===========================================================================
 *                          6. Messaging subsystem
 *===========================================================================*/

static void idt_ntb_inmsg_work(struct work_struct *work);

static void idt_ntb_outmsg_work(struct work_struct *work);

static void idt_ntb_msg_tasklet(unsigned long data);

/*
 * Constructor is used initialize the allocated message structure
 */
static inline void idt_ntb_msg_ctor(struct idt_ntb_msg *msg)
{
	/* Set initial message retry count */
	msg->retry = IDT_NTB_SENDMSG_RETRY;

	/* Init the queue entry */
	INIT_LIST_HEAD(&msg->entry);
}

/*
 * Initialize the messaging subsystem
 */
static int idt_ntb_init_msg(struct idt_ntb_data *pdata)
{
	void __iomem *cfg = pdata->cfg_mmio;
	struct idt_ntb_dev *ndev;
	unsigned char id;

	/* Allocate the IDT messages cache without alignment and flags with no
	 * constructor */
	pdata->msg_cache = kmem_cache_create(NTB_CACHENAME,
		sizeof(struct idt_ntb_msg), 0, 0, NULL);
	if (NULL == pdata->msg_cache) {
		dev_err_data(pdata,
			"IDT NTB failed to allocate the messages cache");
		return -ENOMEM;
	}

	/* Init the messages routing spin lock */
	spin_lock_init(&pdata->msg_lock);

	/* Walk through all the device initializing the message related
	 * structures */
	for (id = 0; id < pdata->peer_cnt; id++) {
		/* Get the current NTB device structure */
		ndev = &pdata->ndevs[id];

		/* Initialize the incoming messages queue */
		atomic_queue_init(&ndev->qinmsg);
		/* Setup the incoming message work thread (it's not delayed) */
		INIT_WORK(&ndev->inmsg_work, idt_ntb_inmsg_work);

		/* Initialize the outgoing messages queue */
		atomic_queue_init(&ndev->qoutmsg);
		/* Setup the outgoing message work thread (it can be
		 * delayed) */
		INIT_DELAYED_WORK(&ndev->outmsg_work, idt_ntb_outmsg_work);
	}

	/* Setup the messages tasklet - bh handler of incoming messages */
	tasklet_init(&pdata->msg_tasklet, idt_ntb_msg_tasklet,
		     (unsigned long)pdata);

	/* Clear the outbound and inbound Messages status */
	idt_ntb_writereg(cfg, IDT_NT_PCI_MSGSTS, MSG_MASK);

	/* Unmask the message interrupts only for the first incoming message
	 * register */
	idt_ntb_writereg(cfg, IDT_NT_PCI_MSGSTSMSK, MSG_UNMASK);

	dev_dbg_data(pdata, "IDT NTB device messaging subsystem initialized");

	return SUCCESS;
}

/*
 * Deinitialize the messaging subsystem
 */
static void idt_ntb_deinit_msg(struct idt_ntb_data *pdata)
{
	void __iomem *cfg = pdata->cfg_mmio;
	struct idt_ntb_dev *ndev;
	struct list_head *entry;
	unsigned char id;

	/* Just kill the tasklet */
	tasklet_kill(&pdata->db_tasklet);

	/* Walk through all the devices deinitializing the message related
	 * structures */
	for (id = 0; id < pdata->peer_cnt; id++) {
		/* Get the current NTB device structure */
		ndev = &pdata->ndevs[id];

		/* Stop the incoming message work thread */
		cancel_work_sync(&ndev->inmsg_work);
		/* Free all the allocated incoming message objects */
		while (!atomic_queue_empty(&ndev->qinmsg)) {
			entry = atomic_queue_get(&ndev->qinmsg);
			kmem_cache_free(pdata->msg_cache,
					to_msg_list_entry(entry));
		}

		/* Stop the outgoing message work thread */
		cancel_delayed_work_sync(&ndev->outmsg_work);
		/* Free all the allocated outgoing message objects */
		while (!atomic_queue_empty(&ndev->qoutmsg)) {
			entry = atomic_queue_get(&ndev->qoutmsg);
			kmem_cache_free(pdata->msg_cache,
					to_msg_list_entry(entry));
		}
	}

	/* Mask the message interrupts */
	idt_ntb_writereg(cfg, IDT_NT_PCI_MSGSTSMSK, MSG_MASK);

	/* Clear the outbound and inbound messages status */
	idt_ntb_writereg(cfg, IDT_NT_PCI_MSGSTS, MSG_MASK);

	/* Destroy the IDT messages cache */
	kmem_cache_destroy(pdata->msg_cache);

	dev_dbg_data(pdata,
		"IDT NTB function messaging subsystem deinitialized");
}

/*
 * Write message to the specified peer
 */
static int idt_ntb_writemsg(struct idt_ntb_dev *ndev, const struct ntb_msg *msg)
{
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	void __iomem *cfg = to_cfg_ndev(ndev);
	u32 stat, swpmsgctl[IDT_NTB_MSG_CNT];
	int regid;

	/* Initialize the message control register so the local outbound message
	 * registers would be connected with the peers inbound ones */
	for (regid = 0; regid < IDT_NTB_MSG_CNT; regid++) {
		/* Init switch partition message control registers variable */
		swpmsgctl[regid] = 0;
		idt_ntb_writefld_var(&swpmsgctl[regid], IDT_SW_MSGROUTE_REG,
				     regid);
		idt_ntb_writefld_var(&swpmsgctl[regid], IDT_SW_MSGROUTE_PART,
				     ndev->part);
	}

	/* Use spin lock to synchronize just thirteen IO operations. It's used
	 * just among the kernel threads so we don't need to disable IRQs/bh */
	spin_lock(&pdata->msg_lock);
	/* Route to the local outbound message to the inbound one of the peer
	 * and send the data to there starting from the data because the
	 * interrupts are enabled for the first message register only */
	for (regid = (IDT_NTB_MSG_CNT - 1); 0 <= regid; regid--) {
		/* Set the route and send the data */
		idt_ntb_writereg(cfg, partdata_tbl[pdata->part].msgctl[regid],
				 swpmsgctl[regid]);
		idt_ntb_writereg(cfg, IDT_NT_PCI_OUTMSG0 + regid,
				 msg->data[regid]);
		/* Read the status of the previous operation */
		stat = idt_ntb_readfld_mem(cfg, IDT_NT_OUTMSGSTS);
		if (SUCCESS != stat) {
			dev_dbg_ndev(ndev,
				"Failed to send message to peer %hhd", regid);
			break;
		}
	}
	/* Immedietly clear the outbound message status if it has been set */
	if (SUCCESS != stat) {
		idt_ntb_writereg(cfg, IDT_NT_PCI_MSGSTS, OUTMSG_MASK);
	}
	/* Finally unlock the message routing subsystem */
	spin_unlock(&pdata->msg_lock);

	/* If the write operation was not successful then the peer inbound
	 * register must be full so return -EBUSY error */
	if (SUCCESS != stat) {
		return -EBUSY;
	}

	return SUCCESS;
}

/*
 * Read the message
 */
static int idt_ntb_readmsg(struct idt_ntb_data *pdata, unsigned char *part,
			   struct ntb_msg *msg)
{
	void __iomem *cfg = pdata->cfg_mmio;
	u32 msgsts, msgsrc;
	unsigned char regid;

	/* Read the inbound messages status */
	msgsts = idt_ntb_readfld_mem(cfg, IDT_NT_INMSGSTS);
	if (INMSG_STS != msgsts) {
		dev_err_data(pdata, "Invalid status %#80x to read msg", msgsts);
		BUG();
		return -EINVAL;
	}

	/* Read data from the inbound message registers. It doesn't need to be
	 * synchronized since the read operation is performed from the tasklet
	 * only, that is non-reentrant */
	*part = idt_ntb_readreg(cfg, IDT_NT_PCI_INMSGSRC0);
	for (regid = 0; regid < IDT_NTB_MSG_CNT; regid++) {
		msg->data[regid] =
			idt_ntb_readreg(cfg, IDT_NT_PCI_INMSG0 + regid);
		/* Read the source of the message checking whether the message
		 * data has come from the same partition */
		msgsrc = idt_ntb_readreg(cfg, IDT_NT_PCI_INMSGSRC0 + regid);
		if (msgsrc != *part) {
			dev_err_data(pdata,
				"Message data is inconsistent, src: %u != %u",
				*part, msgsrc);
			BUG();
			return -EINVAL;
		}
	}

	/* Clear the inbound message status */
	idt_ntb_writereg(cfg, IDT_NT_PCI_MSGSTS, INMSG_MASK);

	return SUCCESS;
}

/*
 * Work thread handling the inbound messages events
 */
static void idt_ntb_inmsg_work(struct work_struct *work)
{
	struct idt_ntb_dev *ndev = to_ndev_inmsg_work(work);
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	struct list_head *entry;
	struct idt_ntb_msg *msgwrap;

	/* Retrieve the last received message. It's bug to have inbound message
	 * queue empty at this point since the tasklet has just added one in
	 * there */
	entry = atomic_queue_get(&ndev->qinmsg);
	BUG_ON(NULL == entry);
	msgwrap = to_msg_list_entry(entry);

	/* Call the client driver message event handler */
	ntb_msg_event(&ndev->ntb, NTB_MSG_NEW, &msgwrap->msg);

	/* Message memory can be freed */
	kmem_cache_free(pdata->msg_cache, msgwrap);
}

/*
 * Work thread handling the outgoing messages
 */
static void idt_ntb_outmsg_work(struct work_struct *work)
{
	struct idt_ntb_dev *ndev = to_ndev_outmsg_work(work);
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	struct list_head *entry;
	struct idt_ntb_msg *msgwrap;
	int ret;

	/* Retrieve a message from the top of the queue. It's bug to have
	 * inbound message queue empty at this point since the client driver
	 * has just added one in there */
	entry = atomic_queue_get(&ndev->qoutmsg);
	BUG_ON(NULL == entry);
	msgwrap = to_msg_list_entry(entry);

	/* If link is not up it is useless to send any data */
	if (OFF == idt_ntb_link_status(ndev)) {
		dev_dbg_ndev(ndev,
			"Link got suddenly down while sending a message");
		/* Link got down so rise the fail event */
		ntb_msg_event(&ndev->ntb, NTB_MSG_FAIL, &msgwrap->msg);
		/* Message memory can be freed */
		kmem_cache_free(pdata->msg_cache, msgwrap);
		/* If some messages are left then reschedule the worker */
		goto outmsg_work_requeue;
	} /* else of (ON ==  idt_ntb_link_status(ndev)) */

	/* Try to send the message */
	ret = idt_ntb_writemsg(ndev, &msgwrap->msg);
	if (SUCCESS == ret) {
		/* The message has been successfully sent so rise the event */
		ntb_msg_event(&ndev->ntb, NTB_MSG_SENT, &msgwrap->msg);
		/* Message memory can be freed */
		kmem_cache_free(pdata->msg_cache, msgwrap);
		/* May need to reschedule the worker */
		goto outmsg_work_requeue;
	} /* else if (SUCCESS != ret) {} */

	/* Could not send message. Rise the error if it has been the last
	 * attempt. If it hasn't get the message back into the queue and
	 * restart the worker */
	msgwrap->retry--;
	if (likely(0 != msgwrap->retry)) {
		atomic_queue_add(&ndev->qoutmsg, &msgwrap->entry);
	} else /* if (0 == msgwrap->retry) */ {
		dev_err_ndev(ndev, "Run out of attempt to send a message");
		/* Rise the error in this case */
		ntb_msg_event(&ndev->ntb, NTB_MSG_FAIL, &msgwrap->msg);
		/* Message memory can be freed */
		kmem_cache_free(pdata->msg_cache, msgwrap);
	}

	/* If there is something left to send then queue the handler again */
outmsg_work_requeue:
	if (!atomic_queue_empty(&ndev->qoutmsg)) {
		(void)queue_delayed_work(pdata->idt_wq, &ndev->outmsg_work,
					 IDT_NTB_SENDMSG_TOUT);
	}
}

/*
 * Tasklet handling inbound messages
 */
static void idt_ntb_msg_tasklet(unsigned long data)
{
	struct idt_ntb_data *pdata = (struct idt_ntb_data *)data;
	struct idt_ntb_dev *ndev, *tndev = NULL;
	struct idt_ntb_msg *msgwrap;
	void __iomem *cfg = pdata->cfg_mmio;
	unsigned char part, id;

	/* Allocate the memory for the new message */
	msgwrap = kmem_cache_alloc(pdata->msg_cache, GFP_KERNEL);
	if (NULL == msgwrap) {
		dev_err_data(pdata,
			"Failed to allocate memory for incoming message");
		return;
	}
	/* Initializet the allocated message wrap structure although it's not
	 * necessary here */
	idt_ntb_msg_ctor(msgwrap);

	/* Read the message from the inbound registers. Don't need to check
	 * the return value since error would be asserted anyway */
	(void)idt_ntb_readmsg(pdata, &part, &msgwrap->msg);

	/* Finally unmask the message IRQs so the next message can be
	 * retrieved */
	idt_ntb_writereg(cfg, IDT_NT_PCI_MSGSTSMSK, MSG_UNMASK);

	/* Find device the message has been sent to */
	for (id = 0; id < pdata->peer_cnt; id++) {
		/* Retrieve the current NTB device */
		ndev = &pdata->ndevs[id];

		/* Break the loop if target device is found */
		if (ndev->part == part) {
			tndev = ndev;
			break;
		}
	}
	/* Assert bug if message was received from invalid partition */
	BUG_ON(NULL == tndev);

	/* Add the new message to the tail of incoming queue of the target
	 * device */
	atomic_queue_add_tail(&ndev->qinmsg, &msgwrap->entry);

	/* Schedule the inbound message worker straight away */
	(void)queue_work(pdata->idt_wq, &ndev->inmsg_work);
}

/*
 * NTB bus callback - post the message to the peer
 */
static int idt_ntb_msg_post(struct ntb_dev *ntb, struct ntb_msg *msg)
{
	struct idt_ntb_dev *ndev = to_ndev_ntb(ntb);
	struct idt_ntb_data *pdata = to_data_ndev(ndev);
	struct idt_ntb_msg *msgwrap;
	unsigned char idx;

	/* If the link is down then don't post any message */
	if (OFF == idt_ntb_link_status(ndev)) {
		dev_dbg_ndev(ndev,
			"Can't post a message since link is down");
		return -EINVAL;
	}

	/* Allocate memory for message wrap structure */
	msgwrap = kmem_cache_alloc(pdata->msg_cache, GFP_KERNEL);
	if (NULL == msgwrap) {
		dev_err_data(pdata,
			"Failed to allocate memory for outgoing message");
		return -ENOMEM;
	}
	/* Initializet the allocated message wrap structure */
	idt_ntb_msg_ctor(msgwrap);

	/* Fill in the message wrapper with data */
	for (idx = 0; idx < IDT_NTB_MSG_CNT; idx++) {
		msgwrap->msg.data[idx] = msg->data[idx];
	}

	/* Add the initialized wrap to the queue of outgoing messages */
	atomic_queue_add_tail(&ndev->qoutmsg, &msgwrap->entry);

	/* Start the outgoing messages worker with no timeout */
	(void)queue_delayed_work(pdata->idt_wq, &ndev->outmsg_work, 0);

	return SUCCESS;
}

/*
 * NTB bus callback - size of the message data
 */
static int idt_ntb_msg_size(struct ntb_dev *ntb)
{
	/* Just return the number of messages registers */
	return IDT_NTB_MSG_CNT;
}

/*===========================================================================
 *                          7. IRQ-related functions
 *===========================================================================*/

static irqreturn_t idt_ntb_isr(int irq, void *dev);

/*
 * Convert the temperature field to the value and fraction
 */
static inline void idt_ntb_convert_temp(const u32 temp,
					unsigned char *val, unsigned char *frac)
{
	*val = temp >> 1;
	*frac = ((temp & 0x1) ? 5 : 0);
}

/*
 * Initialize the IDT IRQ sources
 */
static void idt_ntb_init_irqsrc(struct idt_ntb_data *pdata)
{
	void __iomem *cfg = pdata->cfg_mmio;
	u32 tempctl = 0;

	/* Set the temperature sensor alarms */
	idt_ntb_writefld_var(&tempctl, IDT_SW_TMP_LTH, IDT_NTB_TEMP_LTH << 1);
	idt_ntb_writefld_var(&tempctl, IDT_SW_TMP_HTH, IDT_NTB_TEMP_HTH << 1);
	idt_ntb_writefld_var(&tempctl, IDT_SW_TMP_BLTH_EN, ON);
	idt_ntb_writefld_var(&tempctl, IDT_SW_TMP_AHTH_EN, ON);
	idt_ntb_writefld_var(&tempctl, IDT_SW_TMP_PDOWN, OFF);
	idt_ntb_writereg(cfg, IDT_SW_PCI_TMPCTL, tempctl);

	/* Interrupts are enabled by default only for Primary side since there
	 * can be more than one device */
	if (NTB_TOPO_PRI == pdata->role) {
		/* Enable the interrupts of message, doorbells, switch and
		 * temperature sensor events. This will generate all the
		 * pending interrupts after the link is effectively enabled */
		idt_ntb_writereg(cfg, IDT_NT_PCI_NTINTMSK, NTINT_UNMASK);
	} else /* if (NTB_TOPO_SEC == pdata->role) */ {
		/* Disable all the interrupts. NTB device enable callback will
		 * enable the necessary message, doorbells, switch and
		 * temperature sensor events */
		idt_ntb_writereg(cfg, IDT_NT_PCI_NTINTMSK, ALLINT_MASK);
	}
}

/*
 * Clear the IDT IRQs
 */
static void idt_ntb_clear_irqsrc(struct idt_ntb_data *pdata)
{
	void __iomem *cfg = pdata->cfg_mmio;
	u32 tempctl = 0;

	/* Unset the temperature sensor alarm and disable the sensor */
	idt_ntb_writefld_var(&tempctl, IDT_SW_TMP_BLTH_EN, OFF);
	idt_ntb_writefld_var(&tempctl, IDT_SW_TMP_AHTH_EN, OFF);
	idt_ntb_writefld_var(&tempctl, IDT_SW_TMP_PDOWN, ON);
	idt_ntb_writereg(cfg, IDT_SW_PCI_TMPCTL, tempctl);

	/* Mask all the interrupts */
	idt_ntb_writereg(cfg, IDT_NT_PCI_NTINTMSK, ALLINT_MASK);
}

/*
 * Initialize the PCIe interrupt handler
 *
 * NOTE The code is gotoed a bit, but still it's pretty obvious. First
 * we try to enable MSI interrupt. If it fails we initiate the INTx interrupt.
 * In any successful case the IDT NTB interrupts need to be enabled.
 */
static int idt_ntb_init_isr(struct idt_ntb_data *pdata)
{
	struct pci_dev *pdev = pdata->pdev;
	int ret;

	/* Enable the MSI interrupts */
	ret = pci_enable_msi(pdev);
	if (SUCCESS != ret) {
		dev_err_data(pdata, "IDT failed to enable MSI interrupt");
		goto err_try_intx;
	}

	/* Request correspondig IRQ number */
	ret = request_irq(pdev->irq, idt_ntb_isr, 0, NTB_IRQNAME, pdata);
	if (SUCCESS != ret) {
		dev_err_data(pdata, "IDT failed to set MSI IRQ handler");
		goto err_disable_msi;
	}

	/* From now on the MSI interrupt is used */
	dev_dbg_data(pdata, "IDT NTB is using MSI interrupts");

	/* Need to enable the corresponding IDT NTB interrupts */
	goto idt_init_irqs;

err_disable_msi:
	pci_disable_msi(pdev);

err_try_intx:
	/* Enable INTx interrutps since MSI can't be used */
	pci_intx(pdev, ON);

	ret = request_irq(pdev->irq, idt_ntb_isr, IRQF_SHARED,
			  NTB_IRQNAME, pdata);
	if (SUCCESS != ret) {
		dev_err_data(pdata, "IDT failed to enable INTx interrupt");
		goto err_pci_indx;
	}

	/* From now on the INTx interrupt is used */
	dev_dbg_data(pdata, "IDT NTB is using INTx interrupts");

	/* Need to enable the corresponding IDT NTB interrupts */
idt_init_irqs:
	idt_ntb_init_irqsrc(pdata);

	dev_dbg_data(pdata, "IDT NTB function IRQs initilized");

	return SUCCESS;

err_pci_indx:
	pci_intx(pdev, OFF);

	return ret;
}

/*
 * Deinitialize the PCIe interrupt handler
 */
static void idt_ntb_clear_isr(struct idt_ntb_data *pdata)
{
	struct pci_dev *pdev = pdata->pdev;

	/* Clear the IDT NTB interrupt sources by masking them */
	idt_ntb_clear_irqsrc(pdata);

	/* Stop the interrupt handling */
	free_irq(pdev->irq, pdata);
	if (pci_dev_msi_enabled(pdev)) {
		pci_disable_msi(pdev);
	} else /* if (!pci_dev_msi_enabled(pdev)) */ {
		pci_intx(pdev, OFF);
	}

	dev_dbg_data(pdata, "IDT NTB function interrupts are disabled");
}

/*
 * Switch events ISR
 */
static void idt_ntb_se_isr(struct idt_ntb_data *pdata)
{
	void __iomem *cfg = pdata->cfg_mmio;
	u32 ntintsts = 0, sests;

	/* Clean the corresponding interrupt bit */
	idt_ntb_writefld_var(&ntintsts, IDT_NT_SEINT_STS, ON);
	idt_ntb_writereg(cfg, IDT_NT_PCI_NTINTSTS, ntintsts);

	/* Just print we got the switch event */
	sests = idt_ntb_readreg(cfg, IDT_SW_PCI_SESTS);
	dev_dbg_data(pdata, "Got switch event IRQ %#08x", sests);
}

/*
 * Temperature sensor event ISR
 */
static void idt_ntb_temp_isr(struct idt_ntb_data *pdata)
{
	void __iomem *cfg = pdata->cfg_mmio;
	u32 ntintsts = 0, curtemp;
	unsigned char val, frac;

	/* Clean the corresponding interrupt bit */
	idt_ntb_writefld_var(&ntintsts, IDT_NT_TMPINT_STS, ON);
	idt_ntb_writereg(cfg, IDT_NT_PCI_NTINTSTS, ntintsts);

	/* Read the temperature status */
	curtemp = idt_ntb_readfld_mem(cfg, IDT_SW_TMP_CURTEMP);
	idt_ntb_convert_temp(curtemp, &val, &frac);

	/* Print the current temperature */
	dev_warn_data(pdata,
		"IDT temperature sensor alarm: %hhu.%hhu, valid space [%d;%d]",
		val, frac, IDT_NTB_TEMP_LTH, IDT_NTB_TEMP_HTH);

	/* Read the temperature alarm to clear the value out */
	(void)idt_ntb_readreg(cfg, IDT_SW_PCI_TMPALARM);
}

/*
 * IDT PCIe-swtich NTB-function interrupts handler
 */
static irqreturn_t idt_ntb_isr(int irq, void *dev)
{
	struct idt_ntb_data *pdata = dev;
	void __iomem *cfg = pdata->cfg_mmio;
	u32 ntintsts;
	unsigned long setbit;
	irqreturn_t status = IRQ_NONE;

	/* Read the NTINTSTS register to determine the source of the
	 * interrupt.
	 * NOTE In order to make sure the deferred handlers are executed
	 * only when the corresponding interrupt really happens, the
	 * message/boorbell interrupt is temporarily masked. Additionally
	 * the interrupts status register must be filtered with the interrupts
	 * mask since the correposnding bit may be set even when the interrupt
	 * is masked */
	ntintsts = idt_ntb_readreg(cfg, IDT_NT_PCI_NTINTSTS) &
		   ~idt_ntb_readreg(cfg, IDT_NT_PCI_NTINTMSK);
	for_each_set_bit_u32(ntintsts, setbit) {
		/* Handle the cause of the interrupt */
		switch (setbit) {
		case MSGINT_BIT:
			/* Mask the message IRQs until the data is handled. It
			 * must be unmasked within the tasklet right after the
			 * data is read so the next message can be retrieved */
			idt_ntb_writereg(cfg, IDT_NT_PCI_MSGSTSMSK, MSG_MASK);
			/* Schedule the tasklet to handle the new message */
			tasklet_schedule(&pdata->msg_tasklet);
			break;
		case DBINT_BIT:
			/* Mask the doorbell IRQs until the data is handled. It
			 * must be unmasked within the tasklet right after the
			 * doorbell status bits are read and clear so the next
			 * doorbell event can be raised */
			idt_ntb_writereg(cfg, IDT_NT_PCI_INDBELLMSK, INDB_MASK);
			/* Schedule the tasklet to handle the set doorbell bits */
			tasklet_schedule(&pdata->db_tasklet);
			break;
		case SEINT_BIT:
			/* Just call the switch event handler. It doesn't do
			 * much work */
			idt_ntb_se_isr(pdata);
			break;
		case TEMPINT_BIT:
			/* Just call the temperature sensor event handler.
			 * It doesn't do much work */
			idt_ntb_temp_isr(pdata);
			break;
		default:
			dev_err_data(pdata,
				"Invalid IDT IQR status bit is set");
			break;
		}
		/* If there is any interrupt bit is set then we handle it */
		status = IRQ_HANDLED;
	}

	return status;
}

/*===========================================================================
 *                         8. NTB bus initialization
 *===========================================================================*/

/*
 * NTB KAPI operations
 *
 * NOTE This driver implements the synchronous interface only.
 */
static const struct ntb_dev_ops idt_ntb_ops = {
	.link_is_up		= idt_ntb_link_is_up,
	.link_enable		= idt_ntb_link_enable,
	.link_disable		= idt_ntb_link_disable,
	.mw_count		= idt_ntb_mw_count,
	.mw_get_maprsc		= idt_ntb_mw_get_maprsc,
	.mw_get_align		= idt_ntb_mw_get_align,
	.mw_set_trans		= idt_ntb_mw_set_trans,
	.peer_mw_count		= idt_ntb_peer_mw_count,
	.peer_mw_get_align	= idt_ntb_peer_mw_get_align,
	.db_valid_mask		= idt_ntb_db_valid_mask,
	.db_vector_count	= idt_ntb_db_vector_count,
	.db_vector_mask		= idt_ntb_db_vector_mask,
	.db_read		= idt_ntb_db_read,
	.db_set			= idt_ntb_db_set,
	.db_clear		= idt_ntb_db_clear,
	.db_read_mask		= idt_ntb_db_read_mask,
	.db_set_mask		= idt_ntb_db_set_mask,
	.db_clear_mask		= idt_ntb_db_clear_mask,
	.peer_db_set		= idt_ntb_peer_db_set,
	.msg_post		= idt_ntb_msg_post,
	.msg_size		= idt_ntb_msg_size
};

/*
 * NTB devices registration function
 */
static int idt_ntb_register_devs(struct idt_ntb_data *pdata)
{
	struct idt_ntb_dev *ndev;
	int id, ret;

	/* Loop over all the NTB devices initializing the necessary fields */
	for (id = 0; id < pdata->peer_cnt; id++) {
		/* Retrieve the current NTB device */
		ndev = &pdata->ndevs[id];

		/* Set the device operation callbacks */
		ndev->ntb.ops = &idt_ntb_ops;

		/* Register the device */
		ret = ntb_register_device(&ndev->ntb);
		if (SUCCESS != ret) {
			dev_err_data(pdata, "Failed to register NTB device");
			goto err_unregister_device;
		}
	}

	dev_dbg_data(pdata, "IDT NTB device(s) successfully registered");

	return SUCCESS;

err_unregister_device:
	for (id--; 0 <= id; id--) {
		ndev = &pdata->ndevs[id];
		ntb_unregister_device(&ndev->ntb);
	}

	return ret;
}

/*
 * NTB devices unregistration function
 */
static void idt_ntb_unregister_devs(struct idt_ntb_data *pdata)
{
	struct idt_ntb_dev *ndev;
	int id;

	/* Loop over all the NTB devices initializing the necessary fields */
	for (id = 0; id < pdata->peer_cnt; id++) {
		/* Retrieve the current NTB device */
		ndev = &pdata->ndevs[id];

		/* Just unregister the device */
		ntb_unregister_device(&ndev->ntb);
	}

	dev_dbg_data(pdata, "IDT NTB devices are practically unregistered");
}

/*===========================================================================
 *                        9. IDT NT-functions topology
 *===========================================================================*/

/*
 * Add the NT-function pair of Primary and Secondary ports to the topology
 */
static inline void idt_ntb_addntb(struct idt_ntb_topo *topo,
				  const unsigned char pri,
				  const unsigned char sec)
{
	topo->priports |= ((u32)1 << pri);
	topo->secports[pri] |= ((u32)1 << sec);
}

/*
 * Retrieve the port role
 */
static inline enum ntb_topo idt_ntb_portrole(const struct idt_ntb_topo *topo,
					     const unsigned char port)
{
	return ((topo->priports & ((u32)1 << port)) ?
		NTB_TOPO_PRI : NTB_TOPO_SEC);
}

/*
 * Function first checks whether the port can have an NT-function then whether
 * the function is activated on the port
 */
static int idt_ntb_checkport(const struct idt_ntb_data *pdata,
			     const unsigned char port)
{
	void __iomem *cfg = pdata->cfg_mmio;
	unsigned char pid;
	u32 sts, mode;
	int stat = -EINVAL;

	/* Check whether the port can have the NT-function */
	for (pid = 0; pid < pdata->swcfg->port_cnt; pid++) {
		if (pdata->swcfg->ports[pid] == port) {
			stat = SUCCESS;
			break;
		}
	}
	/* Return -EINVAL if it can't */
	if (SUCCESS != stat) {
		return -EINVAL;
	}

	/* Get the port status so to determine the port mode */
	sts = idt_ntb_readreg(cfg, portdata_tbl[port].sts);
	mode = idt_ntb_readfld_var(sts, IDT_SW_PORT_MODE);

	/* Check whther the port has the NT-function */
	if (PORTMODE_NT != mode && PORTMODE_USNT != mode &&
	    PORTMODE_USNTDMA != mode) {
		return -EINVAL;
	}

	return SUCCESS;
}

/*
 * Scan the IDT NT-function topology by reading the NTSDATA register
 * That register is initialized with the Primary port number of the
 * corresponding secondary ports. Of course the algorithm doesn't permit the
 * two Primary ports pointing to each other.
 */
static int idt_ntb_scantopo(struct idt_ntb_data *pdata)
{
	void __iomem *cfg = pdata->cfg_mmio;
	struct idt_ntb_topo *topo = &pdata->topo;
	unsigned char pid;
	unsigned long port;
	u32 priport;
	int ret;

	/* Clean the topo structure */
	memset(topo, 0, sizeof(*topo));

	/* Walk through all the available ports checking whether the
	 * NT-function enabled on them. If so retrieve its Primary side port */
	for (pid = 0; pid < pdata->swcfg->port_cnt; pid++) {
		/* Retrieve the port number */
		port = pdata->swcfg->ports[pid];

		/* Check whether the port has the NT-function
		 * NOTE Within this loop we are sure it can */
		if (SUCCESS == idt_ntb_checkport(pdata, port)) {
			/* If it does then read it's NTSDATA interpreting its
			 * value as the Primary port number */
			priport = idt_ntb_readreg(cfg,
				portdata_tbl[port].ntsdata);

			/* Add the NTB to the topology only if the retrieved
			 * primary port can have NT-function and have it
			 * activated */
			ret = idt_ntb_checkport(pdata, priport);
			if (SUCCESS == ret && port != priport) {
				idt_ntb_addntb(topo, priport, port);
				/* Increment the number of NTB pairs */
				topo->paircnt++;
			}

			/* If the retrieved port either can't have the
			 * NT-function or doesn't have NT-function activated
			 * then the topology is corrupted */
			if (SUCCESS != ret) {
				dev_err_data(pdata,
					"Invalid primary NT port %u was read",
					priport);
				return -EINVAL;
			}
		} /* else { just skip it }*/
	}

	/* Check the topology consistency to make sure it is just downwards
	 * directional tree graph with two levels: one primary root and
	 * a number of secondary lists (can be none) */
	for_each_set_bit_u32(topo->priports, port) {
		/* Check whether there is no any Primary port amongst the
		 * Secondary ports */
		if (topo->secports[port] & topo->priports) {
			dev_err_data(pdata,
				"Port %lu has Primary and Secondary roles,"
				"IDT NTB topology is inconsistent", port);
			return -EINVAL;
		}
	}

	dev_dbg_data(pdata, "IDT NTB functions topology has been scanned");

	return SUCCESS;
}

/*
 * Create set of Secondary sided peer devices of the topology
 * The function is used by the Primary side of the topology
 */
static int idt_ntb_secpeers(struct idt_ntb_data *pdata)
{
	void __iomem *cfg = pdata->cfg_mmio;
	struct idt_ntb_topo *topo = &pdata->topo;
	u32 secports, portsts;
	unsigned char id = 0;
	unsigned long port;
	int node;

	/* Get the set of the Secondary ports of the current Primary port */
	secports = topo->secports[pdata->port];

	/* Calculate the number of peers */
	pdata->peer_cnt = hweight32(secports);

	/* Allocate the memory for all the peers IDT NTB device structures */
	node = dev_to_node(to_dev_data(pdata));
	pdata->ndevs = kzalloc_node(pdata->peer_cnt*sizeof(*pdata->ndevs),
		GFP_KERNEL, node);
	if (IS_ERR_OR_NULL(pdata->ndevs)) {
		dev_err_data(pdata,
			"Failed to allocate memory for Secondary peer devices");
		return -ENOMEM;
	}

	/* Walk through all the secondary ports initializing the
	 * corresponding NTB device and data fields */
	for_each_set_bit_u32(secports, port) {
		/* Read the port status register to retrieve the partition */
		portsts = idt_ntb_readreg(cfg, portdata_tbl[port].sts);

		/* Save the port and partition numbers */
		pdata->ndevs[id].port = port;
		pdata->ndevs[id].part =
			idt_ntb_readfld_var(portsts, IDT_SW_PORT_SWPART);

		/* Initialize the local topology and PCI device fields */
		pdata->ndevs[id].ntb.topo = pdata->role;
		pdata->ndevs[id].ntb.pdev = pdata->pdev;

		/* Increment the device id number */
		id++;
	}

	return SUCCESS;
}

/*
 * Create Primary sided peer device of the topology
 * The function is used by the Secondary side of the topology
 */
static int idt_ntb_pripeer(struct idt_ntb_data *pdata)
{
	void __iomem *cfg = pdata->cfg_mmio;
	u32 priport, portsts;
	int node;

	/* Get the Primary port of the current port */
	priport = idt_ntb_readreg(cfg, portdata_tbl[pdata->port].ntsdata);

	/* There is going to be just one peer */
	pdata->peer_cnt = 1;

	/* Allocate the memory for IDT NTB device structure of just one peer */
	node = dev_to_node(to_dev_data(pdata));
	pdata->ndevs = kzalloc_node(sizeof(*pdata->ndevs), GFP_KERNEL, node);
	if (IS_ERR_OR_NULL(pdata->ndevs)) {
		dev_err_data(pdata,
			"Failed to allocate memory for Primary peer device");
		return -ENOMEM;
	}

	/* Read the port status register to retrieve the partition */
	portsts = idt_ntb_readreg(cfg, portdata_tbl[priport].sts);

	/* Save the peer id, port and partition numbers */
	pdata->ndevs->port = priport;
	pdata->ndevs->part = idt_ntb_readfld_var(portsts, IDT_SW_PORT_SWPART);

	/* Initialize the local topology and PCI device fields */
	pdata->ndevs->ntb.topo = pdata->role;
	pdata->ndevs->ntb.pdev = pdata->pdev;

	return SUCCESS;
}

/*
 * Enumerate the peer pairs
 *
 * Basically the pairid is just the order number of the corresponding
 * Secondary side port. So the function just loop over the Primary ports.
 * If the local port is Primary then just linearly enumerate its peers
 * starting from the corresponding number.
 * If the local port is Secondary then the function walks through
 * all the Secondary port of the corresponding Primary port looking
 * for the current one to assign the simultaniously incremented id.
 */
static void idt_ntb_enumpairs(struct idt_ntb_data *pdata)
{
	struct idt_ntb_topo *topo = &pdata->topo;
	unsigned char id, pairid = 0;
	unsigned long priport, secport;
	u32 secports;

	/* Loop over all the Primary ports calculating the pairids */
	for_each_set_bit_u32(topo->priports, priport) {
		/* Retrieve the Secondary ports connected to the current
		 * Primary one */
		secports = topo->secports[priport];

		/* Enumerate the current port related pairs  */
		/* If current port is Primary then enumerate its peers */
		if (NTB_TOPO_PRI == pdata->role && priport == pdata->port) {
			for (id = 0; id < pdata->peer_cnt; id++) {
				pdata->ndevs[id].pairid = pairid + id;
			}
			/* Stop looping, the job is done */
			break;
		}
		/* If the current port is Secondary then retrieve its peer id
		 * within the corresponding Primary port */
		else if (NTB_TOPO_SEC == pdata->role &&
			 priport == pdata->ndevs[0].port) {
			id = 0;
			for_each_set_bit_u32(secports, secport) {
				if (secport == pdata->port) {
					pdata->ndevs[0].pairid = pairid + id;
					break;
				}
				id++;
			}
			/* Stop looping, the job is done */
			break;
		}

		/* Increment the pairid with the number of the related Secondary
		 * ports */
		pairid += hweight32(secports);
	}
}

/*
 * Create the NTB devices with respect to the topology
 */
static int idt_ntb_addpeers(struct idt_ntb_data *pdata)
{
	void __iomem *cfg = pdata->cfg_mmio;
	struct idt_ntb_topo *topo = &pdata->topo;
	u32 portsts;
	int ret;

	/* Retrieve the current port number */
	pdata->port = idt_ntb_readfld_mem(cfg, IDT_NT_PORTNUM);

	/* Read the current port partition number */
	portsts = idt_ntb_readreg(cfg, portdata_tbl[pdata->port].sts);
	pdata->part = idt_ntb_readfld_var(portsts, IDT_SW_PORT_SWPART);

	/* Check whether the current port role is Primary or Secondary */
	pdata->role = idt_ntb_portrole(topo, pdata->port);

	/* Create either the Primary or Secondary side peers set */
	ret = (NTB_TOPO_PRI == pdata->role) ?
		idt_ntb_secpeers(pdata) : idt_ntb_pripeer(pdata);
	if (SUCCESS != ret) {
		return ret;
	}

	/* Enumerate all the NTB connected pairs */
	idt_ntb_enumpairs(pdata);

	dev_dbg_data(pdata, "IDT NTB peer devices created");

	return SUCCESS;
}

/*
 * Remove the peer NTB devices added to the data structure
 */
static void idt_ntb_delpeers(struct idt_ntb_data *pdata)
{
	/* Release the memory occupied by the */
	kfree(pdata->ndevs);

	dev_dbg_data(pdata, "IDT NTB peer devices discarded");
}

/*===========================================================================
 *                     10. Basic initialization functions
 *===========================================================================*/

/*
 * Check whether the device is properly pre-initialized
 */
static int idt_ntb_check_quirks(struct pci_dev *pdev)
{
	u32 data, fld;
	int ret;

	/* Read the BARSETUP0 */
	ret = pci_read_config_dword(pdev, BARSETUP0_OFF, &data);
	if (SUCCESS != ret) {
		dev_err(&pdev->dev,
			"Failed to read BARSETUP0 configuration register");
		return ret;
	}

	/* Check whether the BAR0 register is enabled */
	if (OFF == idt_ntb_readfld_var(data, IDT_NT_BARSTP_EN)) {
		dev_err(&pdev->dev,
			"BAR0 isn't enabled");
		return -EINVAL;
	}

	/* Check whether the BAR0 maps the registers configuration space */
	fld = idt_ntb_readfld_var(data, IDT_NT_BARSTP_MODE);
	if (BARSTP_MODE_CFGSPC != fld) {
		dev_err(&pdev->dev,
			"BAR0 isn't configured to map the configuration space");
		return -EINVAL;
	}

	/* Read the BARSETUP2 */
	ret = pci_read_config_dword(pdev, BARSETUP2_OFF, &data);
	if (SUCCESS != ret) {
		dev_err(&pdev->dev,
			"Failed to read BARSETUP2 configuration register");
		return ret;
	}

	/* Check whether the BAR2 register is enabled */
	if (OFF == idt_ntb_readfld_var(data, IDT_NT_BARSTP_EN)) {
		dev_err(&pdev->dev,
			"BAR2 isn't enabled");
		return -EINVAL;
	}

	/* Check whether the BAR2 maps memory windows */
	fld = idt_ntb_readfld_var(data, IDT_NT_BARSTP_MODE);
	if (BARSTP_MODE_WNDW != fld) {
		dev_err(&pdev->dev,
			"BAR2 isn't configured to map memory windows");
		return -EINVAL;
	}

	/* Check whether the BAR2 maps the 24-entries lookup table */
	fld = idt_ntb_readfld_var(data, IDT_NT_BARSTP_ATRAN);
	if (BARSTP_ATRAN_LU24 != fld) {
		dev_err(&pdev->dev,
			"BAR2 isn't configured to map 24-entries lookup table");
		return -EINVAL;
	}

	return SUCCESS;
}

/*
 * Create the IDT PCIe-swtich driver data structure performing the basic
 * initialization
 */
static struct idt_ntb_data *idt_ntb_create_data(struct pci_dev *pdev,
						const struct pci_device_id *id)
{
	struct idt_ntb_data *pdata;
	int node;

	/* Allocate the memory at the device NUMA node */
	node = dev_to_node(&pdev->dev);
	pdata = kzalloc_node(sizeof(*pdata), GFP_KERNEL, node);
	if (IS_ERR_OR_NULL(pdata)) {
		dev_err(&pdev->dev,
			"Failed to allocate memory for IDT NTB driver data");
		return ERR_PTR(-ENOMEM);
	}

	/* Create the workqueue used by the driver */
	pdata->idt_wq = create_workqueue(NTB_WQNAME);
	if (IS_ERR_OR_NULL(pdata->idt_wq)) {
		dev_err(&pdev->dev, "Failed to create workqueue");
		goto err_kfree;
	}

	/* Put the IDT driver data pointer to the PCI-device private pointer */
	pci_set_drvdata(pdev, pdata);
	/* Save the PCI-device pointer inside the data structure */
	pdata->pdev = pdev;
	/* Save the IDT PCIe-switch ports configuration */
	pdata->swcfg = (struct idt_89hpes_pdata *)id->driver_data;

	dev_dbg_data(pdata, "IDT NTB device data created");

	return pdata;

err_kfree:
	kfree(pdata);

	return NULL;
}

/*
 * Free the IDT PCie-swtich driver data structure
 */
static void idt_ntb_free_data(struct idt_ntb_data *pdata)
{
	struct pci_dev *pdev = pdata->pdev;

	/* Flush and destroy the workqueue */
	flush_workqueue(pdata->idt_wq);
	destroy_workqueue(pdata->idt_wq);

	/* Clean the private data pointer of the PCI-device structure */
	pci_set_drvdata(pdev, NULL);

	/* Free the memory allocated for the IDT NTB driver data */
	kfree(pdata);

	dev_dbg(&pdev->dev, "IDT NTB device data discarded");
}

/*
 * Initialize the basic PCI-related subsystem
 */
static int idt_ntb_init_pci(struct idt_ntb_data *pdata)
{
	struct pci_dev *pdev = pdata->pdev;
	int ret;

	/* Enable the device advanced error reporting. Don't check the return
	 * value since the service might be disabled from the kernel */
	ret = pci_enable_pcie_error_reporting(pdev);
	if (SUCCESS != ret) {
		dev_err_data(pdata, "Failed to enable AER capability of IDT NTB");
	}
	/* Cleanup the uncorrectable error status before starting the rest of
	 * initialization */
	pci_cleanup_aer_uncorrect_error_status(pdev);

	/* First enable the PCI device */
	ret = pci_enable_device(pdev);
	if (SUCCESS != ret) {
		dev_err_data(pdata, "Failed to enable the PCI device");
		goto err_disable_aer;
	}

	/* Reguest the PCI device resources like the BAR memory mapping, etc
	 * It's done for BAR0 for now */
	ret = pci_request_region(pdev, BAR0, NTB_NAME);
	if (SUCCESS != ret) {
		dev_err_data(pdata,
			"Failed to request the PCI BAR0 resources");
		goto err_disable_device;
	}

	/* Initialize the bit mask of DMA although I don't see where it can be
	 * used for now */
	ret = pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
	if (SUCCESS != ret) {
		ret = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
		if (SUCCESS != ret) {
			dev_err_data(pdata, "Failed to set any DMA bit mask\n");
			goto err_release_region;
		}
		dev_warn_data(pdata, "Cannot set the DMA highmem bit mask\n");
	}
	ret = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64));
	if (SUCCESS != ret) {
		ret = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
		if (SUCCESS != ret) {
			dev_err_data(pdata,
				"Failed to set any consistent DMA bit mask\n");
			goto err_release_region;
		}
		dev_warn_data(pdata,
			"Cannot set the consistent DMA highmem bit mask\n");
	}

	/* Retrieve the virtual address of the PCI configuration space */
	pdata->cfg_mmio = pci_iomap(pdev, BAR0, 0);
	if (IS_ERR_OR_NULL(pdata->cfg_mmio)) {
		dev_err_data(pdata,
			"Failed to map the IDT NT-function config space\n");
		ret = -EIO;
		goto err_release_region;
	}

	dev_dbg_data(pdata, "IDT NTB function PCI interface was initialized");

	return SUCCESS;

err_disable_aer:
	(void)pci_disable_pcie_error_reporting(pdev);
err_release_region:
	pci_release_region(pdev, BAR0);
err_disable_device:
	pci_disable_device(pdev);

	return ret;
}

/*
 * Deinitialize the basic PCI-related subsystem
 */
static void idt_ntb_deinit_pci(struct idt_ntb_data *pdata)
{
	struct pci_dev *pdev = pdata->pdev;

	/* Disable the AER capability */
	(void)pci_disable_pcie_error_reporting(pdev);

	/* Unmap the IDT PCIe-switch configuration space */
	pci_iounmap(pdev, pdata->cfg_mmio);

	/* Release the PCI-device BAR0 resources */
	pci_release_region(pdev, BAR0);

	/* Finally disable the PCI device */
	pci_disable_device(pdev);

	dev_dbg_data(pdata, "IDT NTB function PCI interface was cleaned");
}

/*===========================================================================
 *                      11. DebugFS callback functions
 *===========================================================================*/

static ssize_t idt_ntb_dbgfs_info_read(struct file *filp, char __user *ubuf,
				       size_t count, loff_t *offp);

static ssize_t idt_ntb_dbgfs_ntregs_read(struct file *filp, char __user *ubuf,
					 size_t count, loff_t *offp);

static ssize_t idt_ntb_dbgfs_swregs_read(struct file *filp, char __user *ubuf,
					 size_t count, loff_t *offp);

/*
 * Driver DebugFS info file operations
 */
static const struct file_operations idt_ntb_dbgfs_info_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = idt_ntb_dbgfs_info_read
};

/*
 * Driver DebugFS NT registers file operations
 */
static const struct file_operations idt_ntb_dbgfs_ntregs_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = idt_ntb_dbgfs_ntregs_read
};

/*
 * Driver DebugFS IDT PCIe-swtich global registers file operations
 */
static const struct file_operations idt_ntb_dbgfs_swregs_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = idt_ntb_dbgfs_swregs_read
};

/*
 * DebugFS read info node callback
 */
static ssize_t idt_ntb_dbgfs_info_read(struct file *filp, char __user *ubuf,
				       size_t count, loff_t *offp)
{
	struct idt_ntb_data *pdata = filp->private_data;
	void __iomem *cfg = pdata->cfg_mmio;
	enum ntb_speed speed;
	enum ntb_width width;
	char *strbuf;
	size_t size;
	ssize_t ret = 0, off = 0;
	u32 var;
	int id, sts, part, bdf, port;
	unsigned char temp, frac;

	/* Lets limit the buffer size the way the Intel/AMD drivers do */
	size = min_t(size_t, count, 0x1000U);

	/* Allocate the memory for the buffer */
	strbuf = kmalloc(size, GFP_KERNEL);
	if (NULL == strbuf) {
		return -ENOMEM;
	}

	/* Put the data into the string buffer */
	off += scnprintf(strbuf + off, size - off,
		"\n\t\tIDT PCIe-switch NT-function Information:\n\n");

	/* General device configurations */
	off += scnprintf(strbuf + off, size - off,
		"Switch port\t\t\t- %hhu\n", pdata->port);
	off += scnprintf(strbuf + off, size - off,
		"Port partition\t\t\t- %hhu\n", pdata->part);
	off += scnprintf(strbuf + off, size - off,
		"Number of peers\t\t\t- %hhu\n", pdata->peer_cnt);

	/* Local switch NT-function role topology and available port to
	 * communicate to */
	off += scnprintf(strbuf + off, size - off,
		"NT-function role\t\t- %s\n", ntb_topo_string(pdata->role));
	off += scnprintf(strbuf + off, size - off,
		"Peer Port:Partition available\t- ");
	for (id = 0; id < pdata->peer_cnt; id++) {
		off += scnprintf(strbuf + off, size - off,
			"%hhd:%hhd ",
			pdata->ndevs[id].port, pdata->ndevs[id].part);
	}
	off += scnprintf(strbuf + off, size - off, "\n");

	/* Links status */
	var = idt_ntb_readreg(cfg, portdata_tbl[pdata->port].sts);
	if (idt_ntb_readfld_var(var, IDT_SW_PORT_LNKUP)) {
		off += scnprintf(strbuf + off, size - off,
			"Local Port Link status\t\t- ");
		var = idt_ntb_readreg(cfg, IDT_NT_PCI_PCIELSTS);
		off += scnprintf(strbuf + off, size - off,
			"PCIe Gen %u ",
			idt_ntb_readfld_var(var, IDT_NT_CURLNKSPD));
		off += scnprintf(strbuf + off, size - off,
			"x%u lanes\n",
			idt_ntb_readfld_var(var, IDT_NT_CURLNKWDTH));
	} else {
		off += scnprintf(strbuf + off, size - off,
			"Local port link status\t\t- Down (Weird)\n");
	}
	off += scnprintf(strbuf + off, size - off,
		"Peer ports link status\t\t- ");
	for (id = 0; id < pdata->peer_cnt; id++) {
		sts = idt_ntb_link_is_up(&pdata->ndevs[id].ntb, &speed, &width);
		if (ON == sts) {
			off += scnprintf(strbuf + off, size - off,
			"%hhd:Gen %u x%u, ", pdata->ndevs[id].port, speed, width);
		} else /* if (OFF == sts) */ {
			off += scnprintf(strbuf + off, size - off,
			"%hhd:Down, ", pdata->ndevs[id].port);
		}
	}
	off += scnprintf(strbuf + off, size - off, "\n");

	/* General resources information */
	off += scnprintf(strbuf + off, size - off,
		 "Total doorbells count\t\t- %u\n", IDT_NTB_DBELL_CNT);
	off += scnprintf(strbuf + off, size - off,
		 "Total memory windows count\t- %u\n", IDT_NTB_MW_CNT);
	off += scnprintf(strbuf + off, size - off,
		 "Total message registers count\t- %u\n", IDT_NTB_MSG_CNT);

	/* Common resources state */
	var = idt_ntb_readreg(cfg, IDT_SW_PCI_GDBELLSTS);
	off += scnprintf(strbuf + off, size - off,
			 "Global doorbells status\t\t- %#010x\n", var);
	var = idt_ntb_readreg(cfg, IDT_NT_PCI_INDBELLSTS);
	off += scnprintf(strbuf + off, size - off,
			 "Local doorbells status\t\t- %#010x\n", var);
	off += scnprintf(strbuf + off, size - off,
			 "Mirror doorbells value\t\t- %#010x\n",
			 pdata->db_sts);
	var = idt_ntb_readreg(cfg, IDT_NT_PCI_INDBELLMSK);
	off += scnprintf(strbuf + off, size - off,
			 "Local doorbells mask\t\t- %#010x\n", var);
	off += scnprintf(strbuf + off, size - off,
			 "Mirror doorbells mask value\t- %#010x\n",
			 pdata->db_msk);

	/* Per-device resources */
	for (id = 0; id < pdata->peer_cnt; id++) {
		off += scnprintf(strbuf + off, size - off,
			"Port %hhd (pair id %hhd)\n",
			pdata->ndevs[id].port, pdata->ndevs[id].pairid);
		off += scnprintf(strbuf + off, size - off,
			"\tDoorbells share\t- "
			"local %#010x offset %hhu, peer %#010x offset %hhu\n",
			pdata->ndevs[id].db_self_mask,
			pdata->ndevs[id].db_self_offset,
			pdata->ndevs[id].db_peer_mask,
			pdata->ndevs[id].db_peer_offset);
		off += scnprintf(strbuf + off, size - off,
			"\tDoorbells\t- count %hhu, valid mask: %#010x,\n",
			pdata->ndevs[id].db_cnt,
			pdata->ndevs[id].db_valid_mask);
		off += scnprintf(strbuf + off, size - off,
			"\tMemory windows\t- local/peer count %hhu/%hhu, "
			"size %u bytes, local offset: %hhu\n",
			pdata->ndevs[id].mw_self_cnt,
			pdata->ndevs[id].mw_peer_cnt,
			(unsigned int)pdata->mw_size,
			pdata->ndevs[id].mw_self_offset);
	}

	/* Doorbells mapping */
	off += scnprintf(strbuf + off, size - off,
			 "\nInbound db:part mapping\n\t");
	for (id = 0; id < IDT_NTB_DBELL_CNT; id++) {
		var = idt_ntb_readreg(cfg, IDT_SW_PCI_GIDBELLMSK0 + id);
		off += scnprintf(strbuf + off, size - off, "%02d:", id);
		for_each_set_bit_u32(~var & 0xFF, part) {
			off += scnprintf(strbuf + off, size - off, "%d,", part);
		}
		off += scnprintf(strbuf + off, size - off, "\b; ");
		if (0 == ((id + 1) % 10)) {
			off += scnprintf(strbuf + off, size - off, "\n\t");
		}
	}
	off += scnprintf(strbuf + off, size - off,
			 "\nOutbound db:part mapping\n\t");
	for (id = 0; id < IDT_NTB_DBELL_CNT; id++) {
		var = idt_ntb_readreg(cfg, IDT_SW_PCI_GODBELLMSK0 + id);
		off += scnprintf(strbuf + off, size - off, "%02d:", id);
		for_each_set_bit_u32(~var & 0xFF, part) {
			off += scnprintf(strbuf + off, size - off,
				"%d,", part);
		}
		off += scnprintf(strbuf + off, size - off, "\b; ");
		if (0 == ((id + 1) % 10)) {
			off += scnprintf(strbuf + off, size - off, "\n\t");
		}
	}
	off += scnprintf(strbuf + off, size - off, "\n");

	/* NTB control register */
	var = idt_ntb_readreg(cfg, IDT_NT_PCI_NTCTL);
	off += scnprintf(strbuf + off, size - off,
			 "\nNTB control register\t- %#010x\n", var);

	/* NTB Mapping table */
	off += scnprintf(strbuf + off, size - off,
			 "NTB mapping table\n");
	for (id = 0; id < IDT_NTB_MTBL_ENTRY_CNT; id++) {
		idt_ntb_writereg(cfg, IDT_NT_PCI_NTMTBLADDR, (u32)id);
		var = idt_ntb_readreg(cfg, IDT_NT_PCI_NTMTBLDATA);
		if (ON == idt_ntb_readfld_var(var, IDT_NT_MTBL_VALID)) {
			bdf = idt_ntb_readfld_var(var, IDT_NT_MTBL_BDF);
			off += scnprintf(strbuf + off, size - off,
				"\t%02d: part %d, bus %d, dev %d, func %d\n",
				id, idt_ntb_readfld_var(var, IDT_NT_MTBL_PART),
				(bdf >> 8) & 0xFF, (bdf >> 3) & 0x1F, bdf & 7);
		}
	}

	/* Currently enabled IRQs */
	off += scnprintf(strbuf + off, size - off, "\nNTB interrupts status\n");
	var = idt_ntb_readreg(cfg, IDT_NT_PCI_NTINTMSK);
	for_each_set_bit_u32(ALLINT_MASK, id) {
		switch (id) {
		case MSGINT_BIT:
			off += scnprintf(strbuf + off, size - off,
				"\tMessage interrupts\t\t\t\t- ");
			break;
		case DBINT_BIT:
			off += scnprintf(strbuf + off, size - off,
				"\tDoorbell interrupts\t\t\t\t- ");
			break;
		case SEINT_BIT:
			off += scnprintf(strbuf + off, size - off,
				"\tSwitch event interrupts\t\t\t\t- ");
			break;
		case FMCI_BIT:
			off += scnprintf(strbuf + off, size - off,
				"\tFailover mode change initiated IRQ\t\t- ");
			break;
		case FMCC_BIT:
			off += scnprintf(strbuf + off, size - off,
				"\tFailover mode change completed IRQ\t\t- ");
			break;
		case TEMPINT_BIT:
			off += scnprintf(strbuf + off, size - off,
				"\tTemperature sensor IRQ (T < %d || %d < T)\t- ",
				IDT_NTB_TEMP_LTH, IDT_NTB_TEMP_HTH);
			break;
		default:
			off += scnprintf(strbuf + off, size - off,
				"\tWarning! Invalid bit is set in the NTINTMSK register\n");
			break;
		}

		if (0x0 == (var & BIT_MASK(id))) {
			off += scnprintf(strbuf + off, size - off,
				"enabled\n");
		} else {
			off += scnprintf(strbuf + off, size - off,
				"disabled\n");
		}
	}

	/* Put the data into the string buffer */
	off += scnprintf(strbuf + off, size - off,
		"\n\t\tIDT PCIe-switch general configuration:\n\n");

	/* Boot configuration vector status */
	var = idt_ntb_readreg(cfg, IDT_SW_PCI_BCVSTS);
	off += scnprintf(strbuf + off, size - off,
		"Switch boot mode\n\t");
	switch (idt_ntb_readfld_var(var, IDT_SW_SWMODE)) {
	case (0x0):
		off += scnprintf(strbuf + off, size - off,
			"Single Partition\n");
		break;
	case (0x1):
		off += scnprintf(strbuf + off, size - off,
			"Single Partition with Serial EEPROM\n");
		break;
	case (0x2):
		off += scnprintf(strbuf + off, size - off,
			"Single Partition with Serial EEPROM Jump 0 "
			"Initialization\n");
		break;
	case (0x3):
		off += scnprintf(strbuf + off, size - off,
			"Single Partition with Serial EEPROM Jump 1 "
			"Initialization\n");
		break;
	case (0x8):
		off += scnprintf(strbuf + off, size - off,
			"Single partition with reduced latency\n");
		break;
	case (0x9):
		off += scnprintf(strbuf + off, size - off,
			"Single partition with Serial EEPROM initialization "
			"and reduced latency\n");
		break;
	case (0xA):
		off += scnprintf(strbuf + off, size - off,
			"Multi-partition with Unattached ports\n");
		break;
	case (0xB):
		off += scnprintf(strbuf + off, size - off,
			"Multi-partition with Unattached ports and i2c Reset\n");
		break;
	case (0xC):
		off += scnprintf(strbuf + off, size - off,
			"Multi-partition with Unattached ports and Serial EEPROM "
			"initialization\n");
		break;
	case (0xD):
		off += scnprintf(strbuf + off, size - off,
			"Multi-partition with Unattached ports with i2c Reset "
			"and Serial EEPROM initialization\n");
		break;
	case (0xE):
		off += scnprintf(strbuf + off, size - off,
			"Multi-partition with Disabled ports\n");
		break;
	case (0xF):
		off += scnprintf(strbuf + off, size - off,
			"Multi-partition with Disabled ports and Serial EEPROM "
			"initialization\n");
		break;
	default:
		off += scnprintf(strbuf + off, size - off,
			"Unknown\n");
		break;
	}
	off += scnprintf(strbuf + off, size - off,
		"Switch boot clock mode\n\t");
	switch (idt_ntb_readfld_var(var, IDT_SW_CLKMODE)) {
	case (0x0):
		off += scnprintf(strbuf + off, size - off,
			"Port 0\t\t- non-common global clocked\n"
			"\tOther ports\t- non-common global clocked\n");
		break;
	case (0x1):
		off += scnprintf(strbuf + off, size - off,
			"Port 0\t\t- Common global clocked\n"
			"\tOther ports\t- non-common global clocked\n");
		break;
	case (0x2):
		off += scnprintf(strbuf + off, size - off,
			"Port 0\t\t- non-common global clocked\n"
			"\tOther ports\t- common global clocked\n");
		break;
	case (0x3):
		off += scnprintf(strbuf + off, size - off,
			"Port 0\t\t- common global clocked\n"
			"\tOther ports\t- common global clocked\n");
		break;
	default:
		off += scnprintf(strbuf + off, size - off,
			"Unknown\n");
		break;
	}

	/* Per-port link status and clock configuration */
	off += scnprintf(strbuf + off, size - off,
		"Ports clocking status\n");
	var = idt_ntb_readreg(cfg, IDT_SW_PCI_PCLKMODE);
	for (id = 0; id < pdata->swcfg->port_cnt; id++) {
		port = pdata->swcfg->ports[id];
		sts = idt_ntb_readreg(cfg, portdata_tbl[port].pcielsts);
		off += scnprintf(strbuf + off, size - off,
			"\tPort %d\t- %s %s mode\n", port,
			idt_ntb_readfld_var(sts, IDT_NT_SCLK) ?
			"common" : "non-common",
			idt_ntb_readfld_var(var, IDT_SW_P0CLKMODE + id) ?
			"local" : "global");
	}

	/* SMBus configuration */
	var = idt_ntb_readreg(cfg, IDT_SW_PCI_SMBUSSTS);
	off += scnprintf(strbuf + off, size - off,
		"Slave SMBus address\t- %#04x\n",
		idt_ntb_readfld_var(var, IDT_SW_SSMBADDR));
	off += scnprintf(strbuf + off, size - off,
		"Master SMBus address\t- %#04x\n",
		idt_ntb_readfld_var(var, IDT_SW_MSMBADDR));

	/* Current temperature */
	var = idt_ntb_readfld_mem(cfg, IDT_SW_TMP_CURTEMP);
	idt_ntb_convert_temp(var, &temp, &frac);
	off += scnprintf(strbuf + off, size - off,
		"Switch temperature\t- %d.%dC\n", temp, (0 != frac) ? 5 : 0);

	/* Copy the buffer to the User Space */
	ret = simple_read_from_buffer(ubuf, count, offp, strbuf, off);
	kfree(strbuf);

	return ret;
}

/*
 * Read passed set of registers method for DebugFS nodes
 */
static ssize_t idt_ntb_dbgfs_regs_read(struct file *filp, char __user *ubuf,
				       size_t count, loff_t *offp,
				       enum idt_ntb_cfgreg start,
				       enum idt_ntb_cfgreg end,
				       const char *title)
{
	struct idt_ntb_data *pdata = filp->private_data;
	void __iomem *cfg = pdata->cfg_mmio;
	enum idt_ntb_cfgreg reg;
	enum idt_ntb_regtype regtype;
	ptrdiff_t regoffset;
	enum idt_ntb_regsize regsize;
	const char *regdesc;
	u32 data;
	char *strbuf;
	size_t size;
	ssize_t ret = 0, off = 0;

	/* Lets limit the buffer size the way the Intel/AMD drivers do */
	size = min_t(size_t, count, 0x4000U);

	/* Allocate the memory for the buffer */
	strbuf = kmalloc(size, GFP_KERNEL);
	if (NULL == strbuf) {
		return -ENOMEM;
	}

	/* Put the title first */
	off += scnprintf(strbuf + off, size - off, "\n\t\t%s\n\n", title);

	/* Print the header of the registers */
	off += scnprintf(strbuf + off, size - off, "         03 02 01 00\n");

	/* Scan through the whole passed range reading the addresses, values
	 * and description and printing it to the buffer */
	for (reg = start; reg < end; reg++) {
		/* Retrieve the register type, offset, size and description */
		idt_ntb_regparams(reg, &regtype, &regoffset, &regsize, &regdesc);

		/* Read the value of the corresponding register */
		data = idt_ntb_readreg(cfg, reg);

		/* Print the register offset */
		off += scnprintf(strbuf + off, size - off,
			"0x%05lX: ", (unsigned long)regoffset);

		/* Then print the value of the register in compliance with the
		 * register size */
		switch (regsize) {
		case REGBYTE:
			off += scnprintf(strbuf + off, size - off,
				"         %02hhX", data);
			break;
		case REGWORD:
			off += scnprintf(strbuf + off, size - off,
				"      %02hhX %02hhX", (data >> 8), data);
			break;
		case REGDWORD:
		default:
			off += scnprintf(strbuf + off, size - off,
				"%02hhX %02hhX %02hhX %02hhX",
				(data >> 24), (data >> 16), (data >> 8), data);
			break;
		}


		/* Then description if going to be the last */
		off += scnprintf(strbuf + off, size - off,
			" - %s\n", regdesc);
	}

	/* Copy the buffer to the User Space */
	ret = simple_read_from_buffer(ubuf, count, offp, strbuf, off);
	kfree(strbuf);

	return ret;
}

/*
 * DebugFS read NT-function registers node callback
 */
static ssize_t idt_ntb_dbgfs_ntregs_read(struct file *filp, char __user *ubuf,
					 size_t count, loff_t *offp)
{
	ssize_t size;

	/* Read the values of the NT-related registers */
	size = idt_ntb_dbgfs_regs_read(filp, ubuf, count, offp,
		0, IDT_NTB_CFGREGS_SPLIT, "NT-function registers raw values");

	return size;
}

/*
 * DebugFS read IDT PCIe-switch registers node info callback
 */
static ssize_t idt_ntb_dbgfs_swregs_read(struct file *filp, char __user *ubuf,
					 size_t count, loff_t *offp)
{
	ssize_t size;

	/* Read the values of the IDT PCIe-swtich global registers */
	size = idt_ntb_dbgfs_regs_read(filp, ubuf, count, offp,
		IDT_NTB_CFGREGS_SPLIT + 1, IDT_NTB_CFGREGS_END,
		"IDT PCIe-switch global registers raw values");

	return size;
}

/*
 * Driver DebugFS initialization function
 */
static int idt_ntb_init_dbgfs(struct idt_ntb_data *pdata)
{
	const char *devname;
	struct dentry *dbgfs_info, *dbgfs_ntregs, *dbgfs_swregs;
	int ret = 0;

	/* If the top directory is not created then do nothing */
	if (IS_ERR_OR_NULL(dbgfs_topdir)) {
		dev_info_data(pdata,
			"Top DebugFS directory has not been created for "
			NTB_NAME);
		return PTR_ERR(dbgfs_topdir);
	}

	/* Retrieve the device name */
	devname = dev_name(to_dev_data(pdata));

	/* Create the top directory of the device */
	pdata->dbgfs_dir = debugfs_create_dir(devname, dbgfs_topdir);
	if (IS_ERR(pdata->dbgfs_dir)) {
		dev_dbg_data(pdata, "Could not create the DebugFS dir %s for %s",
			devname, NTB_NAME);
		return PTR_ERR(pdata->dbgfs_dir);
	}

	/* Create the info file node */
	dbgfs_info = debugfs_create_file("info", S_IRUSR,
		pdata->dbgfs_dir, pdata, &idt_ntb_dbgfs_info_ops);
	if (IS_ERR(dbgfs_info)) {
		dev_dbg_data(pdata, "Could not create the DebugFS info node");
		ret = PTR_ERR(dbgfs_info);
		goto err_rm_dir;
	}

	/* Create the NT-registers file node */
	dbgfs_ntregs = debugfs_create_file("ntregs", S_IRUSR,
		pdata->dbgfs_dir, pdata, &idt_ntb_dbgfs_ntregs_ops);
	if (IS_ERR(dbgfs_ntregs)) {
		dev_dbg_data(pdata,
			"Could not create the DebugFS NT-registers node");
		ret = PTR_ERR(dbgfs_ntregs);
		goto err_rm_dir;
	}

	/* Create the NT-registers file node */
	dbgfs_swregs = debugfs_create_file("swregs", S_IRUSR,
		pdata->dbgfs_dir, pdata, &idt_ntb_dbgfs_swregs_ops);
	if (IS_ERR(dbgfs_swregs)) {
		dev_dbg_data(pdata,
			"Could not create the DebugFS global registers node");
		ret = PTR_ERR(dbgfs_swregs);
		goto err_rm_dir;
	}

	dev_dbg_data(pdata, "IDT NTB device DebugFS nodes created");

	return SUCCESS;

	/* Following call will remove all the subfiles in the directory */
err_rm_dir:
	debugfs_remove_recursive(pdata->dbgfs_dir);

	return ret;
}

/*
 * Driver DebugFS deinitialization function
 */
static void idt_ntb_deinit_dbgfs(struct idt_ntb_data *pdata)
{
	debugfs_remove_recursive(pdata->dbgfs_dir);

	dev_dbg_data(pdata, "IDT NTB device DebugFS nodes discarded");
}

/*===========================================================================
 *                       12. PCI bus callback functions
 *===========================================================================*/

/*
 * PCI device probe() callback function
 */
static int idt_ntb_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct idt_ntb_data *pdata;
	int ret;

	/* Check whether the kernel has properly fixed the IDT NTB
	 * function up */
	ret = idt_ntb_check_quirks(pdev);
	if (SUCCESS != ret) {
		return ret;
	}

	/* Allocate the memory for the IDT PCIe-swtich NTB driver data */
	pdata = idt_ntb_create_data(pdev, id);
	if (IS_ERR_OR_NULL(pdata)) {
		return PTR_ERR(pdata);
	}

	/* Initialize the basic PCI subsystem of the device */
	ret = idt_ntb_init_pci(pdata);
	if (SUCCESS != ret) {
		goto err_free_data;
	}

	/* Determine the ports NT-functions predefined topology */
	ret = idt_ntb_scantopo(pdata);
	if (SUCCESS != ret) {
		goto err_deinit_pci;
	}

	/* Add all the peers */
	ret = idt_ntb_addpeers(pdata);
	if (SUCCESS != ret) {
		goto err_deinit_pci;
	}

	/* Initialize the doorbells */
	idt_ntb_init_db(pdata);

	/* Allocate the Memory Window resources */
	ret = idt_ntb_init_mws(pdata);
	if (SUCCESS != ret) {
		goto err_freedb;
	}

	/* Init messaging subsystem */
	ret = idt_ntb_init_msg(pdata);
	if (SUCCESS != ret) {
		goto err_clean_mws;
	}

	/* Start the link polling subsystem */
	idt_ntb_init_link(pdata);

	/* Initialize the PCIe interrupts */
	ret = idt_ntb_init_isr(pdata);
	if (SUCCESS != ret) {
		goto err_clear_link;
	}

	/* Register all the devices on the NTB bus */
	ret = idt_ntb_register_devs(pdata);
	if (SUCCESS != ret) {
		goto err_clear_isr;
	}

	/* Initialize the DebugFS node of the IDT PCIe-switch NTB driver.
	 * Don't pay much attention to this even if it failed */
	(void)idt_ntb_init_dbgfs(pdata);

	/* IDT PCIe-switch NTB driver is finally initialized */
	dev_info_data(pdata, "IDT PCIe-swtich NTB devices are ready");

	/* May the force be with us... */
	return SUCCESS;

err_clear_isr:
	idt_ntb_clear_isr(pdata);
err_clear_link:
	idt_ntb_clear_link(pdata);
/*err_deinit_msg:*/
	idt_ntb_deinit_msg(pdata);
err_clean_mws:
	idt_ntb_clean_mws(pdata);
err_freedb:
	idt_ntb_clean_db(pdata);
/*err_delpeers:*/
	idt_ntb_delpeers(pdata);
err_deinit_pci:
	idt_ntb_deinit_pci(pdata);
err_free_data:
	idt_ntb_free_data(pdata);

	return ret;
}

/*
 * PCI device remove() callback function
 */
static void idt_ntb_pci_remove(struct pci_dev *pdev)
{
	struct idt_ntb_data *pdata = pci_get_drvdata(pdev);

	/* Deinit the DebugFS node */
	idt_ntb_deinit_dbgfs(pdata);

	/* Unregister the devices from the NTB bus */
	idt_ntb_unregister_devs(pdata);

	/* Stop the interrupt handler */
	idt_ntb_clear_isr(pdata);

	/* Stop the link polling subsystem */
	idt_ntb_clear_link(pdata);

	/* Deinitialize the messaging subsystem */
	idt_ntb_deinit_msg(pdata);

	/* Clear the memory windows */
	idt_ntb_clean_mws(pdata);

	/* Free the allocated Doorbells */
	idt_ntb_clean_db(pdata);

	/* Delete the added peer devices */
	idt_ntb_delpeers(pdata);

	/* Deinit the basic PCI subsystem */
	idt_ntb_deinit_pci(pdata);

	/* Free the memory occupied by the data */
	idt_ntb_free_data(pdata);

	/* IDT PCIe-switch NTB driver is finally initialized */
	dev_info(&pdev->dev, "IDT PCIe-swtich NTB devices are unloaded");

	/* Sayonara... */
}

/*
 * IDT PCIe-switch models ports configuration structures
 */
static struct idt_89hpes_pdata idt_89hpes24nt6ag2_config = {
	.port_cnt = 6, .ports = {0, 2, 4, 6, 8, 12}
};
static struct idt_89hpes_pdata idt_89hpes32nt8ag2_config = {
	.port_cnt = 8, .ports = {0, 2, 4, 6, 8, 12, 16, 20}
};
static struct idt_89hpes_pdata idt_89hpes32nt8bg2_config = {
	.port_cnt = 8, .ports = {0, 2, 4, 6, 8, 12, 16, 20}
};
static struct idt_89hpes_pdata idt_89hpes12nt12g2_config = {
	.port_cnt = 3, .ports = {0, 8, 16}
};
static struct idt_89hpes_pdata idt_89hpes16nt16g2_config = {
	.port_cnt = 4, .ports = {0, 8, 12, 16}
};
static struct idt_89hpes_pdata idt_89hpes24nt24g2_config = {
	.port_cnt = 8, .ports = {0, 2, 4, 6, 8, 12, 16, 20}
};
static struct idt_89hpes_pdata idt_89hpes32nt24ag2_config = {
	.port_cnt = 8, .ports = {0, 2, 4, 6, 8, 12, 16, 20}
};
static struct idt_89hpes_pdata idt_89hpes32nt24bg2_config = {
	.port_cnt = 8, .ports = {0, 2, 4, 6, 8, 12, 16, 20}
};

/*
 * PCI-ids table of the supported IDT PCIe-switch devices
 */
static const struct pci_device_id idt_ntb_pci_tbl[] = {
	{IDT_PCI_DEVICE_IDS(89HPES24NT6AG2,  idt_89hpes24nt6ag2_config)},
	{IDT_PCI_DEVICE_IDS(89HPES32NT8AG2,  idt_89hpes32nt8ag2_config)},
	{IDT_PCI_DEVICE_IDS(89HPES32NT8BG2,  idt_89hpes32nt8bg2_config)},
	{IDT_PCI_DEVICE_IDS(89HPES12NT12G2,  idt_89hpes12nt12g2_config)},
	{IDT_PCI_DEVICE_IDS(89HPES16NT16G2,  idt_89hpes16nt16g2_config)},
	{IDT_PCI_DEVICE_IDS(89HPES24NT24G2,  idt_89hpes24nt24g2_config)},
	{IDT_PCI_DEVICE_IDS(89HPES32NT24AG2, idt_89hpes32nt24ag2_config)},
	{IDT_PCI_DEVICE_IDS(89HPES32NT24BG2, idt_89hpes32nt24bg2_config)},
	{0}
};
MODULE_DEVICE_TABLE(pci, idt_ntb_pci_tbl);

/*
 * IDT PCIe-switch NT-function device driver structure definition
 */
static struct pci_driver idt_ntb_pci_driver = {
	.name		= KBUILD_MODNAME,
	.probe		= idt_ntb_pci_probe,
	.remove		= idt_ntb_pci_remove,
	.id_table	= idt_ntb_pci_tbl,
};

static int __init idt_ntb_pci_driver_init(void)
{
	pr_info("%s %s\n", NTB_DESC, NTB_VER);

	/* Create the top DebugFS directory if the FS is initialized */
	if (debugfs_initialized())
		dbgfs_topdir = debugfs_create_dir(KBUILD_MODNAME, NULL);

	/* Register the NTB hardware driver to handle the PCI device */
	return pci_register_driver(&idt_ntb_pci_driver);
}
module_init(idt_ntb_pci_driver_init);

static void __exit idt_ntb_pci_driver_exit(void)
{
	/* Unregister the NTB hardware driver */
	pci_unregister_driver(&idt_ntb_pci_driver);

	/* Discard the top DebugFS directory */
	debugfs_remove_recursive(dbgfs_topdir);
}
module_exit(idt_ntb_pci_driver_exit);

