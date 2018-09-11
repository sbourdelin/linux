ccflags-y := -O3
ccflags-y += -Wframe-larger-than=8192
ccflags-y += -D'pr_fmt(fmt)=KBUILD_MODNAME ": " fmt'
ccflags-$(CONFIG_ZINC_DEBUG) += -DDEBUG

ifeq ($(CONFIG_ZINC_CHACHA20),y)
zinc-y += chacha20/chacha20.o
ifeq ($(CONFIG_X86_64)$(CONFIG_UML),y)
zinc-y += chacha20/chacha20-x86_64.o
CFLAGS_chacha20.o += -include $(srctree)/$(src)/chacha20/chacha20-x86_64-glue.h
endif
ifeq ($(CONFIG_ARM),y)
zinc-y += chacha20/chacha20-arm.o
CFLAGS_chacha20.o += -include $(srctree)/$(src)/chacha20/chacha20-arm-glue.h
endif
ifeq ($(CONFIG_ARM64),y)
zinc-y += chacha20/chacha20-arm64.o
CFLAGS_chacha20.o += -include $(srctree)/$(src)/chacha20/chacha20-arm-glue.h
endif
endif

zinc-y += main.o

obj-$(CONFIG_ZINC) := zinc.o
