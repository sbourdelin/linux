#
# Makefile for the SuperH specific drivers.
#
ifneq ($(CONFIG_RENESAS_SH_INTC),y)
obj-$(CONFIG_SH_INTC)			+= intc/
endif
ifneq ($(CONFIG_COMMON_CLK),y)
obj-$(CONFIG_HAVE_CLK)			+= clk/
endif
obj-$(CONFIG_MAPLE)			+= maple/
obj-$(CONFIG_SUPERHYWAY)		+= superhyway/

obj-y					+= pm_runtime.o
