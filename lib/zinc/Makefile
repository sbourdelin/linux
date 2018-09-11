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
ifeq ($(CONFIG_MIPS)$(CONFIG_CPU_MIPS32_R2),yy)
zinc-y += chacha20/chacha20-mips.o
CFLAGS_chacha20.o += -include $(srctree)/$(src)/chacha20/chacha20-mips-glue.h
endif
endif

ifeq ($(CONFIG_ZINC_POLY1305),y)
zinc-y += poly1305/poly1305.o
ifeq ($(CONFIG_X86_64)$(CONFIG_UML),y)
zinc-y += poly1305/poly1305-x86_64.o
CFLAGS_poly1305.o += -include $(srctree)/$(src)/poly1305/poly1305-x86_64-glue.h
endif
ifeq ($(CONFIG_ARM),y)
zinc-y += poly1305/poly1305-arm.o
CFLAGS_poly1305.o += -include $(srctree)/$(src)/poly1305/poly1305-arm-glue.h
endif
ifeq ($(CONFIG_ARM64),y)
zinc-y += poly1305/poly1305-arm64.o
CFLAGS_poly1305.o += -include $(srctree)/$(src)/poly1305/poly1305-arm-glue.h
endif
ifeq ($(CONFIG_MIPS)$(CONFIG_CPU_MIPS32_R2),yy)
zinc-y += poly1305/poly1305-mips.o
CFLAGS_poly1305.o += -include $(srctree)/$(src)/poly1305/poly1305-mips-glue.h
endif
ifeq ($(CONFIG_MIPS)$(CONFIG_64BIT),yy)
zinc-y += poly1305/poly1305-mips64.o
CFLAGS_poly1305.o += -include $(srctree)/$(src)/poly1305/poly1305-mips-glue.h
endif
endif

zinc-y += main.o

obj-$(CONFIG_ZINC) := zinc.o
