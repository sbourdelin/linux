/*
 * umip.c Emulation for instruction protected by the Intel User-Mode
 * Instruction Prevention. The instructions are:
 *    sgdt
 *    sldt
 *    sidt
 *    str
 *    smsw
 *
 * Copyright (c) 2017, Intel Corporation.
 * Ricardo Neri <ricardo.neri@linux.intel.com>
 */

#include <linux/uaccess.h>
#include <asm/umip.h>
#include <asm/traps.h>
#include <asm/insn.h>
#include <asm/insn-eval.h>
#include <linux/ratelimit.h>

/*
 * == Base addresses of GDT and IDT
 * Some applications to function rely finding the global descriptor table (GDT)
 * and the interrupt descriptor table (IDT) in kernel memory.
 * For x86_32, the selected values do not match any particular hole, but it
 * suffices to provide a memory location within kernel memory.
 *
 * == CRO flags for SMSW
 * Use the flags given when booting, as found in head_32.S
 */

#define CR0_STATE (X86_CR0_PE | X86_CR0_MP | X86_CR0_ET | X86_CR0_NE | \
		   X86_CR0_WP | X86_CR0_AM)
#define UMIP_DUMMY_GDT_BASE 0xfffe0000
#define UMIP_DUMMY_IDT_BASE 0xffff0000

/*
 * Definitions for x86 page fault error code bits. Only a simple
 * pagefault during a write in user context is supported.
 */
#define UMIP_PF_USER BIT(2)
#define UMIP_PF_WRITE BIT(1)

enum umip_insn {
	UMIP_SGDT = 0,	/* opcode 0f 01 ModR/M reg 0 */
	UMIP_SIDT,	/* opcode 0f 01 ModR/M reg 1 */
	UMIP_SLDT,	/* opcode 0f 00 ModR/M reg 0 */
	UMIP_SMSW,	/* opcode 0f 01 ModR/M reg 4 */
	UMIP_STR,	/* opcode 0f 00 ModR/M reg 1 */
};

/**
 * __identify_insn - Identify a UMIP-protected instruction
 * @insn:	Instruction structure with opcode and ModRM byte.
 *
 * From the instruction opcode and the reg part of the ModRM byte, identify,
 * if any, a UMIP-protected instruction.
 *
 * Return: an enumeration of a UMIP-protected instruction; -EINVAL on failure.
 */
static int __identify_insn(struct insn *insn)
{
	/* By getting modrm we also get the opcode. */
	insn_get_modrm(insn);

	/* All the instructions of interest start with 0x0f. */
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
	} else {
		return -EINVAL;
	}
}

/**
 * __emulate_umip_insn - Emulate UMIP instructions with dummy values
 * @insn:	Instruction structure with ModRM byte
 * @umip_inst:	Instruction to emulate
 * @data:	Buffer onto which the dummy values will be copied
 * @data_size:	Size of the emulated result
 *
 * Emulate an instruction protected by UMIP. The result of the emulation
 * is saved in the provided buffer. The size of the results depends on both
 * the instruction and type of operand (register vs memory address). Thus,
 * the size of the result needs to be updated.
 *
 * Result: 0 if success, -EINVAL on failure to emulate
 */
static int __emulate_umip_insn(struct insn *insn, enum umip_insn umip_inst,
			       unsigned char *data, int *data_size)
{
	unsigned long dummy_base_addr;
	unsigned short dummy_limit = 0;
	unsigned int dummy_value = 0;

	switch (umip_inst) {
	/*
	 * These two instructions return the base address and limit of the
	 * global and interrupt descriptor table. The base address can be
	 * 24-bit, 32-bit or 64-bit. Limit is always 16-bit. If the operand
	 * size is 16-bit the returned value of the base address is supposed
	 * to be a zero-extended 24-byte number. However, it seems that a
	 * 32-byte number is always returned in legacy protected mode
	 * irrespective of the operand size.
	 */
	case UMIP_SGDT:
		/* fall through */
	case UMIP_SIDT:
		if (umip_inst == UMIP_SGDT)
			dummy_base_addr = UMIP_DUMMY_GDT_BASE;
		else
			dummy_base_addr = UMIP_DUMMY_IDT_BASE;
		if (X86_MODRM_MOD(insn->modrm.value) == 3) {
			/* SGDT and SIDT do not take register as argument. */
			return -EINVAL;
		}

		memcpy(data + 2, &dummy_base_addr, sizeof(dummy_base_addr));
		memcpy(data, &dummy_limit, sizeof(dummy_limit));
		*data_size = sizeof(dummy_base_addr) + sizeof(dummy_limit);
		break;
	case UMIP_SMSW:
		/*
		 * Even though CR0_STATE contain 4 bytes, the number
		 * of bytes to be copied in the result buffer is determined
		 * by whether the operand is a register or a memory location.
		 */
		dummy_value = CR0_STATE;
		/*
		 * These two instructions return a 16-bit value. We return
		 * all zeros. This is equivalent to a null descriptor for
		 * str and sldt.
		 */
		/* fall through */
	case UMIP_SLDT:
		/* fall through */
	case UMIP_STR:
		/* if operand is a register, it is zero-extended */
		if (X86_MODRM_MOD(insn->modrm.value) == 3) {
			memset(data, 0, insn->opnd_bytes);
			*data_size = insn->opnd_bytes;
		/* if not, only the two least significant bytes are copied */
		} else {
			*data_size = 2;
		}
		memcpy(data, &dummy_value, sizeof(dummy_value));
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/**
 * __force_sig_info_umip_fault - Force a SIGSEGV with SEGV_MAPERR
 * @address:	Address that caused the signal
 * @regs:	Register set containing the instruction pointer
 *
 * Force a SIGSEGV signal with SEGV_MAPERR as the error code. This function is
 * intended to be used to provide a segmentation fault when the result of the
 * UMIP emulation could not be copied to the user space memory.
 *
 * Return: none
 */
static void __force_sig_info_umip_fault(void __user *address,
					struct pt_regs *regs)
{
	siginfo_t info;
	struct task_struct *tsk = current;

	if (show_unhandled_signals && unhandled_signal(tsk, SIGSEGV)) {
		printk_ratelimited("%s[%d] umip emulation segfault ip:%lx sp:%lx error:%lx in %lx\n",
				   tsk->comm, task_pid_nr(tsk), regs->ip,
				   regs->sp, UMIP_PF_USER | UMIP_PF_WRITE,
				   regs->ip);
	}

	tsk->thread.cr2		= (unsigned long)address;
	tsk->thread.error_code	= UMIP_PF_USER | UMIP_PF_WRITE;
	tsk->thread.trap_nr	= X86_TRAP_PF;

	info.si_signo	= SIGSEGV;
	info.si_errno	= 0;
	info.si_code	= SEGV_MAPERR;
	info.si_addr	= address;
	force_sig_info(SIGSEGV, &info, tsk);
}

/**
 * fixup_umip_exception - Fixup #GP faults caused by UMIP
 * @regs:	Registers as saved when entering the #GP trap
 *
 * The instructions sgdt, sidt, str, smsw, sldt cause a general protection
 * fault if with CPL > 0 (i.e., from user space). This function can be
 * used to emulate the results of the aforementioned instructions with
 * dummy values. Results are copied to user-space memory as indicated by
 * the instruction pointed by EIP using the registers indicated in the
 * instruction operands. This function also takes care of determining
 * the address to which the results must be copied.
 */
bool fixup_umip_exception(struct pt_regs *regs)
{
	struct insn insn;
	unsigned char buf[MAX_INSN_SIZE];
	/* 10 bytes is the maximum size of the result of UMIP instructions */
	unsigned char dummy_data[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	unsigned long seg_base;
	int not_copied, nr_copied, reg_offset, dummy_data_size;
	void __user *uaddr;
	unsigned long *reg_addr;
	enum umip_insn umip_inst;

	/*
	 * Use the segment base in case user space used a different code
	 * segment, either in protected (e.g., from an LDT) or virtual-8086
	 * modes. In most of the cases seg_base will be zero as in USER_CS.
	 */
	seg_base = insn_get_seg_base(regs, &insn, offsetof(struct pt_regs, ip),
				     true);
	not_copied = copy_from_user(buf, (void __user *)(seg_base + regs->ip),
				    sizeof(buf));
	nr_copied = sizeof(buf) - not_copied;
	/*
	 * The copy_from_user above could have failed if user code is protected
	 * by a memory protection key. Give up on emulation in such a case.
	 * Should we issue a page fault?
	 */
	if (!nr_copied)
		return false;

	insn_init(&insn, buf, nr_copied, 0);

	/*
	 * Override the default operand and address sizes to what is specified
	 * in the code segment descriptor. The instruction decoder only sets
	 * the address size it to either 4 or 8 address bytes and does nothing
	 * for the operand bytes. This OK for most of the cases, but we could
	 * have special cases where, for instance, a 16-bit code segment
	 * descriptor is used.
	 * If there are overrides, the instruction decoder correctly updates
	 * these values, even for 16-bit defaults.
	 */
	insn.addr_bytes = insn_get_seg_default_address_bytes(regs);
	insn.opnd_bytes = insn_get_seg_default_operand_bytes(regs);

	if (!insn.addr_bytes || !insn.opnd_bytes)
		return false;

#ifdef CONFIG_X86_64
	if (user_64bit_mode(regs))
		return false;
#endif

	insn_get_length(&insn);
	if (nr_copied < insn.length)
		return false;

	umip_inst = __identify_insn(&insn);
	/* Check if we found an instruction protected by UMIP */
	if (umip_inst < 0)
		return false;

	if (__emulate_umip_insn(&insn, umip_inst, dummy_data, &dummy_data_size))
		return false;

	/* If operand is a register, write directly to it */
	if (X86_MODRM_MOD(insn.modrm.value) == 3) {
		reg_offset = insn_get_reg_offset_modrm_rm(&insn, regs);
		reg_addr = (unsigned long *)((unsigned long)regs + reg_offset);
		memcpy(reg_addr, dummy_data, dummy_data_size);
	} else {
		uaddr = insn_get_addr_ref(&insn, regs);
		nr_copied = copy_to_user(uaddr, dummy_data, dummy_data_size);
		if (nr_copied  > 0) {
			/*
			 * If copy fails, send a signal and tell caller that
			 * fault was fixed up
			 */
			__force_sig_info_umip_fault(uaddr, regs);
			return true;
		}
	}

	/* increase IP to let the program keep going */
	regs->ip += insn.length;
	return true;
}
