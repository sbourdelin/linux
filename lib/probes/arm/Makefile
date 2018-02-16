ccflags-y += -I$(srctree)/arch/arm/probes/uprobes/
obj-$(CONFIG_UPROBES)		+= decode.o decode-arm.o actions-arm.o
obj-$(CONFIG_KPROBES)		+= decode.o
ifndef CONFIG_THUMB2_KERNEL
obj-$(CONFIG_KPROBES)		+= decode-arm.o
endif
