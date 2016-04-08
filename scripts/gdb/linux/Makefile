always := gdb-scripts

SRCTREE := $(abspath $(srctree))

$(obj)/gdb-scripts:
ifneq ($(KBUILD_SRC),)
	$(Q)ln -fsn $(SRCTREE)/$(obj)/*.py $(objtree)/$(obj)
endif
	@:

clean-files := *.pyc *.pyo $(if $(KBUILD_SRC),*.py)
