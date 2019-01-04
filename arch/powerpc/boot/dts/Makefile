# SPDX-License-Identifier: GPL-2.0

subdir-y += fsl

dtstree		:= $(srctree)/$(src)
dtb-$(CONFIG_PPC) := $(patsubst $(dtstree)/%.dts,%.dtb, $(wildcard $(dtstree)/*.dts))
