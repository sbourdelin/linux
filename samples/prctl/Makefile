ifndef CROSS_COMPILE
obj-$(CONFIG_SAMPLES_PRCTL) := disable-tsc-ctxt-sw-stress-test.o disable-tsc-on-off-stress-test.o disable-tsc-test.o

HOSTCFLAGS_disable-tsc-ctxt-sw-stress-test.o += -I$(objtree)/usr/include
HOSTCFLAGS_disable-tsc-on-off-stress-test.o += -I$(objtree)/usr/include
HOSTCFLAGS_disable-tsc-test.o += -I$(objtree)/usr/include
endif
