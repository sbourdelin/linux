# SPDX-License-Identifier: GPL-2.0
# scripts/dtc makefile

hostprogs-y	:= dtc
always		:= $(hostprogs-y)

dtc-objs	:= dtc.o flattree.o fstree.o data.o livetree.o treesource.o \
		   srcpos.o checks.o util.o
dtc-objs	+= dtc-lexer.lex.o dtc-parser.tab.o

# Source files need to get at the userspace version of libfdt_env.h to compile

KBUILD_HOSTCFLAGS_DTC := -I$(src) -I$(src)/libfdt

KBUILD_HOSTCFLAGS_checks.o := $(KBUILD_HOSTCFLAGS_DTC)
KBUILD_HOSTCFLAGS_data.o := $(KBUILD_HOSTCFLAGS_DTC)
KBUILD_HOSTCFLAGS_dtc.o := $(KBUILD_HOSTCFLAGS_DTC)
KBUILD_HOSTCFLAGS_flattree.o := $(KBUILD_HOSTCFLAGS_DTC)
KBUILD_HOSTCFLAGS_fstree.o := $(KBUILD_HOSTCFLAGS_DTC)
KBUILD_HOSTCFLAGS_livetree.o := $(KBUILD_HOSTCFLAGS_DTC)
KBUILD_HOSTCFLAGS_srcpos.o := $(KBUILD_HOSTCFLAGS_DTC)
KBUILD_HOSTCFLAGS_treesource.o := $(KBUILD_HOSTCFLAGS_DTC)
KBUILD_HOSTCFLAGS_util.o := $(KBUILD_HOSTCFLAGS_DTC)

KBUILD_HOSTCFLAGS_dtc-lexer.lex.o := $(KBUILD_HOSTCFLAGS_DTC)
KBUILD_HOSTCFLAGS_dtc-parser.tab.o := $(KBUILD_HOSTCFLAGS_DTC)

# dependencies on generated files need to be listed explicitly
$(obj)/dtc-lexer.lex.o: $(obj)/dtc-parser.tab.h
