# SPDX-License-Identifier: GPL-2.0
# Makefile for intel specific clk

obj-y += clk-cgu-api.o
ifneq ($(CONFIG_INTEL_GRX500_CLK),)
	obj-y += clk-grx500.o
endif
