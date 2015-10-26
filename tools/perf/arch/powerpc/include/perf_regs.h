#ifndef ARCH_PERF_REGS_H
#define ARCH_PERF_REGS_H

#include <stdlib.h>
#include <linux/types.h>
#include <asm/perf_regs.h>

void perf_regs_load(u64 *regs);

#define PERF_REGS_MASK  ((1ULL << PERF_REG_POWERPC_MAX) - 1)
#define PERF_REGS_MAX   PERF_REG_POWERPC_MAX
#define PERF_SAMPLE_REGS_ABI   PERF_SAMPLE_REGS_ABI_64

#define PERF_REG_IP     PERF_REG_POWERPC_NIP
#define PERF_REG_SP     PERF_REG_POWERPC_R1

static inline const char *perf_reg_name(int id)
{
	switch (id) {
	case PERF_REG_POWERPC_GPR0:
		return "gpr0";
	case PERF_REG_POWERPC_GPR1:
		return "gpr1";
	case PERF_REG_POWERPC_GPR2:
		return "gpr2";
	case PERF_REG_POWERPC_GPR3:
		return "gpr3";
	case PERF_REG_POWERPC_GPR4:
		return "gpr4";
	case PERF_REG_POWERPC_GPR5:
		return "gpr5";
	case PERF_REG_POWERPC_GPR6:
		return "gpr6";
	case PERF_REG_POWERPC_GPR7:
		return "gpr7";
	case PERF_REG_POWERPC_GPR8:
		return "gpr8";
	case PERF_REG_POWERPC_GPR9:
		return "gpr9";
	case PERF_REG_POWERPC_GPR10:
		return "gpr10";
	case PERF_REG_POWERPC_GPR11:
		return "gpr11";
	case PERF_REG_POWERPC_GPR12:
		return "gpr12";
	case PERF_REG_POWERPC_GPR13:
		return "gpr13";
	case PERF_REG_POWERPC_GPR14:
		return "gpr14";
	case PERF_REG_POWERPC_GPR15:
		return "gpr15";
	case PERF_REG_POWERPC_GPR16:
		return "gpr16";
	case PERF_REG_POWERPC_GPR17:
		return "gpr17";
	case PERF_REG_POWERPC_GPR18:
		return "gpr18";
	case PERF_REG_POWERPC_GPR19:
		return "gpr19";
	case PERF_REG_POWERPC_GPR20:
		return "gpr20";
	case PERF_REG_POWERPC_GPR21:
		return "gpr21";
	case PERF_REG_POWERPC_GPR22:
		return "gpr22";
	case PERF_REG_POWERPC_GPR23:
		return "gpr23";
	case PERF_REG_POWERPC_GPR24:
		return "gpr24";
	case PERF_REG_POWERPC_GPR25:
		return "gpr25";
	case PERF_REG_POWERPC_GPR26:
		return "gpr26";
	case PERF_REG_POWERPC_GPR27:
		return "gpr27";
	case PERF_REG_POWERPC_GPR28:
		return "gpr28";
	case PERF_REG_POWERPC_GPR29:
		return "gpr29";
	case PERF_REG_POWERPC_GPR30:
		return "gpr30";
	case PERF_REG_POWERPC_GPR31:
		return "gpr31";
	case PERF_REG_POWERPC_NIP:
		return "nip";
	case PERF_REG_POWERPC_MSR:
		return "msr";
	case PERF_REG_POWERPC_ORIG_R3:
		return "orig_r3";
	case PERF_REG_POWERPC_CTR:
		return "ctr";
	case PERF_REG_POWERPC_LNK:
		return "link";
	case PERF_REG_POWERPC_XER:
		return "xer";
	case PERF_REG_POWERPC_CCR:
		return "ccr";
#ifdef __powerpc64__
	case PERF_REG_POWERPC_SOFTE:
		return "softe";
#else
	case PERF_REG_POWERPC_MQ:
		return "mq";
#endif
	case PERF_REG_POWERPC_TRAP:
		return "trap";
	case PERF_REG_POWERPC_DAR:
		return "dar";
	case PERF_REG_POWERPC_DSISR:
		return "dsisr";
	case PERF_REG_POWERPC_RESULT:
		return "result";
	default:
		return NULL;
	}
	return NULL;
}
#endif /*ARCH_PERF_REGS_H */
