#
# Makefile for the Linux SuperH-specific device drivers.
#

obj-y		+= dma/

ifndef CONFIG_SH_DEVICE_TREE
obj-$(CONFIG_PCI)		+= pci/
endif
obj-$(CONFIG_SUPERHYWAY)	+= superhyway/
obj-$(CONFIG_PUSH_SWITCH)	+= push-switch.o
obj-$(CONFIG_HEARTBEAT)		+= heartbeat.o
