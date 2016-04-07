GCC_PLUGINS_DIR := $(shell $(CC) -print-file-name=plugin)

ifeq ($(PLUGINCC),$(HOSTCC))
HOSTLIBS := hostlibs
HOST_EXTRACFLAGS += -I$(GCC_PLUGINS_DIR)/include -I$(src) -std=gnu99 -ggdb
export HOST_EXTRACFLAGS
else
HOSTLIBS := hostcxxlibs
HOST_EXTRACXXFLAGS += -I$(GCC_PLUGINS_DIR)/include -I$(src) -std=gnu++98 -fno-rtti -fno-exceptions -fasynchronous-unwind-tables -ggdb -Wno-narrowing -Wno-unused-variable
export HOST_EXTRACXXFLAGS
endif

export GCCPLUGINS_DIR HOSTLIBS

$(HOSTLIBS)-$(CONFIG_GCC_PLUGIN_CYC_COMPLEXITY) := cyc_complexity_plugin.so
$(HOSTLIBS)-$(CONFIG_GCC_PLUGIN_SANCOV) += sancov_plugin.so

always := $($(HOSTLIBS)-y)

cyc_complexity_plugin-objs := cyc_complexity_plugin.o
sancov_plugin-objs := sancov_plugin.o
