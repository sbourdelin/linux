# SPDX-License-Identifier: GPL-2.0
out := arch/$(SRCARCH)/include/generated/asm
uapi := arch/$(SRCARCH)/include/generated/uapi/asm

_dummy := $(shell [ -d '$(uapi)' ] || mkdir -p '$(uapi)') \
	  $(shell [ -d '$(out)' ] || mkdir -p '$(out)')

syscall := $(srctree)/$(src)/syscall.tbl

syshdr := $(srctree)/$(src)/syscallhdr.sh
systbl := $(srctree)/$(src)/syscalltbl.sh

quiet_cmd_syshdr = SYSHDR  $@
      cmd_syshdr = $(CONFIG_SHELL) '$(syshdr)' '$<' '$@'  \
		   '$(syshdr_abi_$(basetarget))'          \
		   '$(syshdr_pfx_$(basetarget))'          \
		   '$(syshdr_offset_$(basetarget))'

quiet_cmd_systbl = SYSTBL  $@
      cmd_systbl = $(CONFIG_SHELL) '$(systbl)' '$<' '$@'  \
		   '$(systbl_abi_$(basetarget))'

$(uapi)/unistd_32.h: $(syscall) $(syshdr)
	$(call if_changed,syshdr)

$(out)/syscall_table.h: $(syscall) $(systbl)
	$(call if_changed,systbl)

uapisyshdr-y			+= unistd_32.h
syshdr-y			+= syscall_table.h

targets	+= $(uapisyshdr-y) $(syshdr-y)

PHONY += all
all: $(addprefix $(uapi)/,$(uapisyshdr-y))
all: $(addprefix $(out)/,$(syshdr-y))
	@:
