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

#include <linux/uaccess.h>
#include <asm/umip.h>
#include <asm/traps.h>
#include <asm/insn.h>
#include <asm/insn-kernel.h>
#include <linux/ratelimit.h>

/*
 * == Base addresses of GDT and IDT
 * Some applications to function rely finding the global descriptor table (GDT)
 * and the interrupt descriptor table (IDT) in kernel memory. For x86_64 this
 * matches a memory hole as detailed in Documentation/x86/x86_64/mm.txt.
 * For x86_32, it does not match any particular hole, but it suffices
 * to provide a memory location within kernel memory.
 *
 * == CRO flags for SMSW
 * Use the flags given when booting, as found in head_64/32.S
 */

#define CR0_FLAGS (X86_CR0_PE | X86_CR0_MP | X86_CR0_ET | X86_CR0_NE | \
		   X86_CR0_WP | X86_CR0_AM)
#ifdef CONFIG_X86_64
#define UMIP_DUMMY_GDT_BASE 0xfffffffffffe0000
#define UMIP_DUMMY_IDT_BASE 0xffffffffffff0000
#define CR0_STATE (CR0_FLAGS | X86_CR0_PG)
#else
#define UMIP_DUMMY_GDT_BASE 0xfffe0000
#define UMIP_DUMMY_IDT_BASE 0xffff0000
#define CR0_STATE CR0_FLAGS
#endif

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
	 * 32-bit or 64-bit. Limit is always 16-bit.
	 */
	case UMIP_SGDT:
	case UMIP_SIDT:
		if (umip_inst == UMIP_SGDT)
			dummy_base_addr = UMIP_DUMMY_GDT_BASE;
		else
			dummy_base_addr = UMIP_DUMMY_IDT_BASE;
		if (X86_MODRM_MOD(insn->modrm.value) == 3) {
			/* SGDT and SIDT do not take register as argument. */
			return -EINVAL;
		}
		/* Fill most significant byte with zeros if 16-bit addressing*/
		if (insn->addr_bytes == 2)
			dummy_base_addr &= 0xffffff;
		memcpy(data + 2, &dummy_base_addr, sizeof(dummy_base_addr));
		memcpy(data, &dummy_limit, sizeof(dummy_limit));
		*data_size = sizeof(dummy_base_addr) + sizeof(dummy_limit);
		break;
	case UMIP_SMSW:
		dummy_value = CR0_STATE;
	/*
	 * These two instructions return a 16-bit value. We return
	 * all zeros. This is equivalent to a null descriptor for
	 * str and sldt.
	 */
	case UMIP_SLDT:
	case UMIP_STR:
		/* if operand is a register, it is zero-extended*/
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

static void __force_sig_info_umip_fault(void __user *address,
					struct pt_regs *regs)
{
	siginfo_t info;
	struct task_struct *tsk = current;

	if (show_unhandled_signals && unhandled_signal(tsk, SIGSEGV)) {
		printk_ratelimited("%s[%d] umip emulation segfault ip:%lx sp:%lx error:%lx in %lx\n",
		       tsk->comm, task_pid_nr(tsk), regs->ip,
		       regs->sp, UMIP_PF_USER | UMIP_PF_WRITE, regs->ip);
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

bool fixup_umip_exception(struct pt_regs *regs)
{
	struct insn insn;
	unsigned char buf[MAX_INSN_SIZE];
	/* 10 bytes is the maximum size of the result of UMIP instructions */
	unsigned char dummy_data[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
#ifdef CONFIG_X86_64
	int x86_64 = user_64bit_mode(regs);
#else
	int x86_64 = 0;
#endif
	int not_copied, nr_copied, reg_offset, dummy_data_size;
	void __user *uaddr;
	unsigned long *reg_addr;
	enum umip_insn umip_inst;

	if (v8086_mode(regs))
		/*
		 * In virtual-8086 mode the segment selector cs does not point
		 * to a segment descriptor but is used directly to compute the
		 * address. This is done after shifting it 4 bytes to the left.
		 * The result is added to the instruction pointer.
		 */
		not_copied = copy_from_user(buf,
					    (void __user *)((regs->cs << 4) +
							    regs->ip),
					    sizeof(buf));
	else
		not_copied = copy_from_user(buf, (void __user *)regs->ip,
					    sizeof(buf));
	nr_copied = sizeof(buf) - not_copied;
	/*
	 * The copy_from_user above could have failed if user code is protected
	 * by a memory protection key. Give up on emulation in such a case.
	 * Should we issue a page fault?
	 */
	if (!nr_copied)
		return false;
	insn_init(&insn, buf, nr_copied, x86_64);
	/*
	 * Set address and operand sizes to the default of 16 bits. If
	 * overrides are found, sizes will be fixed when parsing the instruction
	 * prefixes.
	 */
	if (v8086_mode(regs)) {
		insn.addr_bytes = 2;
		insn.opnd_bytes = 2;
	}
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
		reg_offset = insn_get_reg_offset_rm(&insn, regs);
		reg_addr = (unsigned long *)((unsigned long)regs + reg_offset);
		memcpy(reg_addr, dummy_data, dummy_data_size);
	} else {
		uaddr = insn_get_addr_ref(&insn, regs);
		nr_copied = copy_to_user(uaddr, dummy_data, dummy_data_size);
		if (nr_copied  > 0) {
			/*
			 * If copy fails, send a signal and tell caller that
			 * fault was fixed up.
			 */
			__force_sig_info_umip_fault(uaddr, regs);
			return true;
		}
	}

	/* increase IP to let the program keep going */
	regs->ip += insn.length;
	return true;
}
