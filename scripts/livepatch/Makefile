hostprogs-y		:= klp-convert
always			:= $(hostprogs-y)

klp-convert-objs	:= klp-convert.o elf.o

HOSTCFLAGS		:= -g -I$(INSTALL_HDR_PATH)/include -Wall
HOSTLDFLAGS		:= -lelf
