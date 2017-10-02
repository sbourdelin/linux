OBJECT_FILES_NON_STANDARD := y

purgatory-y := entry.o

targets += $(purgatory-y)
PURGATORY_OBJS = $(addprefix $(obj)/,$(purgatory-y))

LDFLAGS_purgatory.ro := -e purgatory_start -r --no-undefined \
					-nostdlib -z nodefaultlib
targets += purgatory.ro

$(obj)/purgatory.ro: $(PURGATORY_OBJS) FORCE
		$(call if_changed,ld)

targets += kexec_purgatory.c

CMD_BIN2C = $(objtree)/scripts/basic/bin2c
quiet_cmd_bin2c = BIN2C $@
	cmd_bin2c = $(CMD_BIN2C) kexec_purgatory < $< > $@

$(obj)/kexec_purgatory.c: $(obj)/purgatory.ro FORCE
	$(call if_changed,bin2c)

obj-${CONFIG_KEXEC_FILE}	+= kexec_purgatory.o
