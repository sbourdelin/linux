# SPDX-License-Identifier: GPL-2.0

ifneq '$(CONFIG_RISCV_BUILTIN_DTB)' '""'
BUILTIN_DTB := $(patsubst "%",%,$(CONFIG_RISCV_BUILTIN_DTB)).dtb.o
else
BUILTIN_DTB :=
endif

obj-$(CONFIG_OF) += $(BUILTIN_DTB)

clean-files := *.dtb *.dtb.S
