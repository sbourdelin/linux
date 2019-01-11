# SPDX-License-Identifier: GPL-2.0
PLUGINCC := $(CONFIG_PLUGIN_HOSTCC:"%"=%)
GCC_PLUGINS_DIR := $(shell $(CC) -print-file-name=plugin)

ifeq ($(PLUGINCC),$(HOSTCC))
  quiet_cmd_plugin = HOSTCC  $@
  pluginc_flags = $(hostc_flags) -std=gnu99
else
  quiet_cmd_plugin = HOSTCXX $@
  pluginc_flags = $(hostcxx_flags) -std=gnu++98 -fno-rtti -fno-exceptions \
	-fasynchronous-unwind-tables -Wno-narrowing -Wno-unused-variable
endif

pluginc_flags += -I$(GCC_PLUGINS_DIR)/include -I$(src) -ggdb -fPIC

$(obj)/randomize_layout_plugin.so: $(objtree)/$(obj)/randomize_layout_seed.h
quiet_cmd_create_randomize_layout_seed = GENSEED $@
cmd_create_randomize_layout_seed = \
  $(CONFIG_SHELL) $(srctree)/$(src)/gen-random-seed.sh $@ $(objtree)/include/generated/randomize_layout_hash.h
$(objtree)/$(obj)/randomize_layout_seed.h: FORCE
	$(call if_changed,create_randomize_layout_seed)
targets = randomize_layout_seed.h randomize_layout_hash.h

extra-y := $(GCC_PLUGIN)

cmd_plugin = $(PLUGINCC) $(pluginc_flags) -shared $(hostld_flags) \
	     -o $@ $< $(hostld_libs)

$(obj)/%.so: $(src)/%.c FORCE
	$(call if_changed_dep,plugin)

hostprogs-y += dummy_to_include_scripts/Makefile.host

clean-files += *.so
