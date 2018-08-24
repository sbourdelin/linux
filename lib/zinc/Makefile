ccflags-y := -O3
ccflags-y += -Wframe-larger-than=8192
ccflags-y += -D'pr_fmt(fmt)=KBUILD_MODNAME ": " fmt'

ifeq ($(CONFIG_ZINC_CHACHA20),y)
zinc-y += chacha20/chacha20.o
endif

zinc-y += main.o

obj-$(CONFIG_ZINC) := zinc.o
