obj-y += testcases.dtb.o
obj-y += ot.dtb.o
obj-y += ot_bad_phandle.dtb.o
obj-y += ot_base.dtb.o

targets += testcases.dtb testcases.dtb.S
targets += ot.dtb ot.dtb.S
targets += ot_bad_phandle.dtb ot_bad_phandle.dtb.S
targets += ot_base.dtb ot_base.dtb.S

.PRECIOUS: \
	$(obj)/%.dtb.S \
	$(obj)/%.dtb

# enable creation of __symbols__ node
DTC_FLAGS_ot := -@
DTC_FLAGS_ot_base := -@
DTC_FLAGS_ot_bad_phandle := -@
