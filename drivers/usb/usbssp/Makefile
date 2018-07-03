# SPDX-License-Identifier: GPL-2.0
# define_trace.h needs to know how to find our header
CFLAGS_trace.o := -I$(src)

obj-$(CONFIG_USB_USBSSP_GADGET)	+=  usbssp-plat.o gadget-dbg.o \
				    gadget-mem.o gadget-ring.o \
				    gadget.o gadget-ep0.o gadget-port.o

ifneq ($(CONFIG_TRACING),)
	usbssp-y		+= trace.o
endif

ifneq ($(CONFIG_DEBUG_FS),)
	usbssp-y		+= gadget-debugfs.o
endif

obj-$(CONFIG_USB_USBSSP_PCI)	+=usbssp-pci-of-wrap.o
