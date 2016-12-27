TARGET:=atlantic

CC = gcc

ifeq "$(CC)" "gcc"
	ccflags-y := -Wall
endif

ifneq ($(KERNELRELEASE),)
	$(TARGET)-objs:=aq_main.o aq_nic.o aq_pci_func.o aq_nic.o aq_vec.o \
	aq_ring.o aq_hw_utils.o aq_ethtool.o hw_atl/hw_atl_a0.o \
	hw_atl/hw_atl_utils.o hw_atl/hw_atl_llh.o

	obj-m:=$(TARGET).o
else
	ifndef KDIR
		BUILD_DIR:=/lib/modules/$(shell uname -r)/build
	else
		BUILD_DIR:=$(KDIR)
	endif

	PWD:=$(shell pwd)

all:
	$(MAKE) -j4 CC=$(CC) -C $(BUILD_DIR) M=$(PWD) modules

dox:	.doxygen
	@doxygen $<

clean:
	$(MAKE) -j4 -C $(BUILD_DIR) M=$(PWD) clean
	@-rm -rdf doc/html 2 > /dev/null

load:
	insmod ./$(TARGET).ko

unload:
	rmmod ./$(TARGET).ko

endif
