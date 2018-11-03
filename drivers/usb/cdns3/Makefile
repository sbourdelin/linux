obj-$(CONFIG_USB_CDNS3)			+= cdns3.o
obj-$(CONFIG_USB_CDNS3_PCI_WRAP)	+= cdns3-pci.o

cdns3-y					:= core.o drd.o debugfs.o
cdns3-$(CONFIG_USB_CDNS3_GADGET)	+= gadget.o ep0.o debug.o
cdns3-$(CONFIG_USB_CDNS3_HOST)		+= host.o
cdns3-pci-y		 		:= cdns3-pci-wrap.o
