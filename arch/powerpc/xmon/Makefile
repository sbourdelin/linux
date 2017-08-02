# Makefile for xmon

subdir-ccflags-$(CONFIG_PPC_WERROR) := -Werror

GCOV_PROFILE := n
UBSAN_SANITIZE := n

ccflags-$(CONFIG_PPC64) := $(NO_MINIMAL_TOC)

ifdef CONFIG_FUNCTION_TRACER
CFLAGS_REMOVE_xmon.o	= -mno-sched-epilog $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_nonstdio.o = -mno-sched-epilog $(CC_FLAGS_FTRACE)
ifdef CONFIG_XMON_DISASSEMBLY
CFLAGS_REMOVE_ppc-dis.o	= -mno-sched-epilog $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_ppc-opc.o	= -mno-sched-epilog $(CC_FLAGS_FTRACE)
ifdef CONFIG_SPU_BASE
CFLAGS_REMOVE_spu-dis.o	= -mno-sched-epilog $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_spu-opc.o	= -mno-sched-epilog $(CC_FLAGS_FTRACE)
endif
endif
endif

obj-y			+= xmon.o nonstdio.o spr_access.o

ifdef CONFIG_XMON_DISASSEMBLY
obj-y			+= ppc-dis.o ppc-opc.o
obj-$(CONFIG_SPU_BASE)	+= spu-dis.o spu-opc.o
endif
