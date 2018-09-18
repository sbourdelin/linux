ccflags-y := -O3
ccflags-y += -Wframe-larger-than=$(if (CONFIG_KASAN),16384,8192)
ccflags-y += -D'pr_fmt(fmt)="zinc: " fmt'
ccflags-$(CONFIG_ZINC_DEBUG) += -DDEBUG
