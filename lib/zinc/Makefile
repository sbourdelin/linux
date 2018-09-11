ccflags-y := -O3
ccflags-y += -Wframe-larger-than=8192
ccflags-y += -D'pr_fmt(fmt)=KBUILD_MODNAME ": " fmt'
ccflags-$(CONFIG_ZINC_DEBUG) += -DDEBUG

zinc-y += main.o

obj-$(CONFIG_ZINC) := zinc.o
