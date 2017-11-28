dtb-$(CONFIG_MSCC_OCELOT)	+= ocelot_pcb123.dtb

obj-y				+= $(patsubst %.dtb, %.dtb.o, $(dtb-y))

# Force kbuild to make empty built-in.o if necessary
obj-				+= dummy.o
