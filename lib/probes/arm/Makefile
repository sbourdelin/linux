ccflags-y	+= -I$(srctree)/lib/uprobes/arm/
ifdef CONFIG_ARM64
ccflags-y += -I$(srctree)/arch/arm64/kernel/probes/
else
ccflags-y += -I$(srctree)/arch/arm/probes/uprobes/
endif
obj-$(CONFIG_UPROBES)		+= decode.o decode-arm.o actions-arm.o
obj-$(CONFIG_KPROBES)		+= decode.o
ifndef CONFIG_THUMB2_KERNEL
obj-$(CONFIG_KPROBES)		+= decode-arm.o
endif
