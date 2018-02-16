# SPDX-License-Identifier: GPL-2.0
ccflags-y	+= -I$(srctree)/lib/probes/arm/
ccflags-y	+= -I$(srctree)/arch/arm64/kernel/probes/
obj-$(CONFIG_KPROBES)		+= kprobes.o decode-insn.o	\
				   kprobes_trampoline.o		\
				   simulate-insn.o
obj-$(CONFIG_UPROBES)		+= uprobes.o decode-insn.o	\
				   simulate-insn.o
