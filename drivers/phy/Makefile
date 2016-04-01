#
# Makefile for the phy drivers.
#

obj-$(CONFIG_GENERIC_PHY)		+= phy-core.o
obj-$(CONFIG_PHY_LPC18XX_USB_OTG)	+= phy-lpc18xx-usb-otg.o
obj-$(CONFIG_PHY_MT65XX_USB3)		+= phy-mt65xx-usb3.o
obj-$(CONFIG_PHY_XGENE)			+= phy-xgene.o
obj-$(CONFIG_PHY_PISTACHIO_USB)		+= phy-pistachio-usb.o

# Phy drivers for different platforms.
obj-$(CONFIG_PHY_PLAT)		+= bcom/	\
				   hisi/	\
				   marvell/	\
				   qcom/	\
				   rcar/	\
				   rockchip/	\
				   samsung/	\
				   st/		\
				   sunxi/	\
				   ti/
