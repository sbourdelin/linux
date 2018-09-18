ccflags-y := -O3
ccflags-y += -Wframe-larger-than=$(if (CONFIG_KASAN),16384,8192)
ccflags-y += -D'pr_fmt(fmt)="zinc: " fmt'
ccflags-$(CONFIG_ZINC_DEBUG) += -DDEBUG

zinc_chacha20-y := chacha20/chacha20.o
zinc_chacha20-$(CONFIG_ZINC_ARCH_X86_64) += chacha20/chacha20-x86_64.o
zinc_chacha20-$(CONFIG_ZINC_ARCH_ARM) += chacha20/chacha20-arm.o
zinc_chacha20-$(CONFIG_ZINC_ARCH_ARM64) += chacha20/chacha20-arm64.o
zinc_chacha20-$(CONFIG_ZINC_ARCH_MIPS) += chacha20/chacha20-mips.o
obj-$(CONFIG_ZINC_CHACHA20) += zinc_chacha20.o

zinc_poly1305-y := poly1305/poly1305.o
zinc_poly1305-$(CONFIG_ZINC_ARCH_X86_64) += poly1305/poly1305-x86_64.o
zinc_poly1305-$(CONFIG_ZINC_ARCH_ARM) += poly1305/poly1305-arm.o
zinc_poly1305-$(CONFIG_ZINC_ARCH_ARM64) += poly1305/poly1305-arm64.o
obj-$(CONFIG_ZINC_POLY1305) += zinc_poly1305.o
