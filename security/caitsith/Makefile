obj-y += permission.o gc.o policy_io.o realpath.o lsm.o load_policy.o

$(obj)/policy/policy.conf:
	@mkdir -p $(obj)/policy/
	@echo Creating an empty policy/policy.conf
	@touch $@

$(obj)/builtin-policy.h: $(obj)/policy/policy.conf
	@echo Generating built-in policy for CaitSith.
	@echo "static char cs_builtin_policy[] __initdata =" > $@.tmp
	@sed -e 's/\\/\\134/g' -e 's/"/\\"/g' -e 's/\(.*\)/"\1\\n"/' < $(obj)/policy/policy.conf >> $@.tmp
	@echo "\"\";" >> $@.tmp
	@mv $@.tmp $@

$(obj)/policy_io.o: $(obj)/builtin-policy.h
