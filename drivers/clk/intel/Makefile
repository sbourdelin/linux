# SPDX-License-Identifier: GPL-2.0
# Makefile for intel specific clk

obj-$(CONFIG_INTEL_CGU_CLK) += clk-cgu.o clk-cgu-pll.o
ifneq ($(CONFIG_INTEL_GRX500_CGU_CLK),)
	obj-y += clk-grx500.o
endif
