# SPDX-License-Identifier: GPL-2.0
subdir-ccflags-y := -I$(srctree)/lib/probes/arm/

obj-$(CONFIG_UPROBES)		+= uprobes/
obj-$(CONFIG_KPROBES)		+= kprobes/
ifdef CONFIG_THUMB2_KERNEL
obj-$(CONFIG_KPROBES)		+= decode-thumb.o
endif
