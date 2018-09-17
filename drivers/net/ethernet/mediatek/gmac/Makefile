# GPL-2.0
#
# Makefile for the MediaTek network device drivers
#

obj-$(CONFIG_MTK_GMAC) += mtk-gmac.o

mtk-gmac-objs := mtk-gmac-net.o \
		mtk-gmac-desc.o \
		mtk-gmac-common.o \
		mtk-gmac-hw.o \
		mtk-gmac-ethtool.o \
		mtk-gmac-ptp.o \
		mtk-gmac-mdio.o

mtk-gmac-${CONFIG_MT2712_GMAC} += mt2712-platform.o
