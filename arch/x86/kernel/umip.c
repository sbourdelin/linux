/*
 * umip.c Emulation for instruction protected by the Intel User-Mode
 * Instruction Prevention. The instructions are:
 *    sgdt
 *    sldt
 *    sidt
 *    str
 *    smsw
 *
 * Copyright (c) 2016, Intel Corporation.
 * Ricardo Neri <ricardo.neri@linux.intel.com>
 */

#include <linux/compiler.h>
#include <linux/bug.h>
#include <linux/uaccess.h>
#include <linux/err.h>
#include <asm/ptrace.h>
#include <asm/umip.h>
#include <linux/thread_info.h>
#include <linux/thread_info.h>

/*
 * The address of this dummy values need to be readable by
 * the user space
 */

static const long umip_dummy_gdt_base;
static const long umip_dummy_idt_base;

enum umip_insn {
	UMIP_SGDT = 0,	/* opcode 0f 01 ModR/M reg 0 */
	UMIP_SIDT,	/* opcode 0f 01 ModR/M reg 1 */
	UMIP_SLDT,	/* opcode 0f 00 ModR/M reg 0 */
	UMIP_SMSW,	/* opcode 0f 01 ModR/M reg 4 */
	UMIP_STR,	/* opcode 0f 00 ModR/M reg 1 */
};

static int __identify_insn(struct insn *insn)
{
	/* by getting modrm we also get the opcode */
	insn_get_modrm(insn);
	if (insn->opcode.bytes[0] != 0xf)
		return -EINVAL;

	if (insn->opcode.bytes[1] == 0x1) {
		switch (X86_MODRM_REG(insn->modrm.value)) {
		case 0:
			return UMIP_SGDT;
		case 1:
			return UMIP_SIDT;
		case 4:
			return UMIP_SMSW;
		default:
			return -EINVAL;
		}
	} else if (insn->opcode.bytes[1] == 0x0) {
		if (X86_MODRM_REG(insn->modrm.value) == 0)
			return UMIP_SLDT;
		else if (X86_MODRM_REG(insn->modrm.value) == 1)
			return UMIP_STR;
		else
			return -EINVAL;
	}
}

static int __emulate_umip_insn(struct insn *insn, enum umip_insn umip_inst,
			       unsigned char *data, int *data_size)
{
	unsigned long const *dummy_base_addr;
	unsigned short dummy_limit = 0;
	unsigned short dummy_value = 0;

	switch (umip_inst) {
	/*
	 * These two instructions return the base address and limit of the
	 * global and interrupt descriptor table. The base address can be
	 * 32-bit or 64-bit. Limit is always 16-bit.
	 */
	case UMIP_SGDT:
	case UMIP_SIDT:
		if (umip_inst == UMIP_SGDT)
			dummy_base_addr = &umip_dummy_gdt_base;
		else
			dummy_base_addr = &umip_dummy_idt_base;
		if (X86_MODRM_MOD(insn->modrm.value) == 3) {
			WARN_ONCE(1, "SGDT cannot take register as argument!\n");
			return -EINVAL;
		}
		/* 16-bit operand. fill most significant byte with zeros */
		if (insn->opnd_bytes == 2)
			dummy_base_addr = (unsigned long *)
					  ((unsigned long)
					   dummy_base_addr & 0xffffff);
		memcpy(data + 2, &dummy_base_addr, sizeof(dummy_base_addr));
		memcpy(data, &dummy_limit, sizeof(dummy_limit));
		*data_size = sizeof(dummy_base_addr) + sizeof(dummy_limit);
		break;
	/*
	 * These three instructions return a 16-bit value. We return
	 * all zeros. This is equivalent to a null descriptor for
	 * str and sldt. For smsw, is equivalent to an all-zero CR0.
	 */
	case UMIP_SLDT:
	case UMIP_SMSW:
	case UMIP_STR:
		/* if operand is a register, it is zero-extended*/
		if (X86_MODRM_MOD(insn->modrm.value) == 3) {
			memset(data, 0, insn->opnd_bytes);
			*data_size = insn->opnd_bytes;
		} else
			*data_size = sizeof(dummy_value);
		memcpy(data, &dummy_value, sizeof(dummy_value));
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int fixup_umip_exception(struct pt_regs *regs)
{
	struct insn insn;
	unsigned char buf[MAX_INSN_SIZE];
	/* 10 bytes is the maximum size of the result of UMIP instructions */
	unsigned char dummy_data[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	int x86_64 = !test_thread_flag(TIF_IA32);
	int not_copied, nr_copied, reg_offset, dummy_data_size;
	void __user *uaddr;
	unsigned long *reg_addr;
	enum umip_insn umip_inst;

	not_copied = copy_from_user(buf, (void __user *)regs->ip, sizeof(buf));
	nr_copied = sizeof(buf) - not_copied;
	/*
	 * The decoder _should_ fail nicely if we pass it a short buffer.
	 * But, let's not depend on that implementation detail.  If we
	 * did not get anything, just error out now.
	 */
	if (!nr_copied)
		return -EFAULT;
	insn_init(&insn, buf, nr_copied, x86_64);
	insn_get_length(&insn);
	if (nr_copied < insn.length)
		return -EFAULT;

	umip_inst = __identify_insn(&insn);
	/* Check if we found an instruction protected by UMIP */
	if (umip_inst < 0)
		return -EINVAL;

	if (__emulate_umip_insn(&insn, umip_inst, dummy_data, &dummy_data_size))
		return -EINVAL;

	/* If operand is a register, write directly to it */
	if (X86_MODRM_MOD(insn.modrm.value) == 3) {
		reg_offset = get_reg_offset_rm(&insn, regs);
		reg_addr = (unsigned long *)((unsigned long)regs + reg_offset);
		memcpy(reg_addr, dummy_data, dummy_data_size);
	} else {
		uaddr = insn_get_addr_ref(&insn, regs);
		nr_copied = copy_to_user(uaddr, dummy_data, dummy_data_size);
		if (nr_copied  > 0)
			return -EFAULT;
	}

	/* increase IP to let the program keep going */
	regs->ip += insn.length;
	return 0;
}
