#
#
obj-y				:= 169445-init.o \
				   169445-int.o 169445-setup.o \
				   169445-time.o

obj-$(CONFIG_EARLY_PRINTK)	+= 169445-console.o

CFLAGS_169445-setup.o = -I$(src)/../../../scripts/dtc/libfdt
