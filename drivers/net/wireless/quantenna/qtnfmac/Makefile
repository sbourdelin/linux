#
# Copyright (c) 2015-2016 Quantenna Communications, Inc.
# All rights reserved.
#

ccflags-y += -Idrivers/net/wireless/quantenna/include
ccflags-y += -D__CHECK_ENDIAN

obj-$(CONFIG_QTNFMAC) += qtnfmac.o

qtnfmac-objs += \
	core.o \
	init.o \
	commands.o \
	trans.o \
	cfg80211.o \
	event.o \
	util.o \
	qlink_util.o

obj-$(CONFIG_QTNFMAC_PCIE) += qtnfmac_pcie.o
qtnfmac_pcie-objs += \
	pcie.o \
	shm_ipc.o
