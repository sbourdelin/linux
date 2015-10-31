# Built-in dtb

ifneq ($(CONFIG_ARC_BUILTIN_DTB_NAME),"")
	builtindtb-y	:= $(patsubst "%",%,$(CONFIG_ARC_BUILTIN_DTB_NAME))
	obj-y   += $(builtindtb-y).dtb.o
	targets += $(builtindtb-y).dtb
endif

.SECONDARY: $(obj)/$(builtindtb-y).dtb.S

dtbs:  $(addprefix  $(obj)/, $(builtindtb-y).dtb)

clean-files := *.dtb  *.dtb.S
