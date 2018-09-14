# SPDX-License-Identifier: GPL-2.0
out := arch/$(SRCARCH)/include/generated/asm
uapi := arch/$(SRCARCH)/include/generated/uapi/asm

_dummy := $(shell [ -d '$(uapi)' ] || mkdir -p '$(uapi)') \
	  $(shell [ -d '$(out)' ] || mkdir -p '$(out)')

syscall32 := $(srctree)/$(src)/syscall_32.tbl
syscall64 := $(srctree)/$(src)/syscall_64.tbl

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

$(uapi)/unistd_32.h: $(syscall32) $(syshdr)
	$(call if_changed,syshdr)

$(uapi)/unistd_64.h: $(syscall64) $(syshdr)
	$(call if_changed,syshdr)

systbl_abi_syscall_table_32 := 32
$(out)/syscall_table_32.h: $(syscall32) $(systbl)
	$(call if_changed,systbl)

systbl_abi_syscall_table_64 := 64
$(out)/syscall_table_64.h: $(syscall64) $(systbl)
	$(call if_changed,systbl)

systbl_abi_syscall_table_c32 := c32
$(out)/syscall_table_c32.h: $(syscall32) $(systbl)
	$(call if_changed,systbl)

uapisyshdr-y			+= unistd_32.h unistd_64.h
syshdr-y			+= syscall_table_32.h syscall_table_64.h \
				   syscall_table_c32.h

targets	+= $(uapisyshdr-y) $(syshdr-y)

PHONY += all
all: $(addprefix $(uapi)/,$(uapisyshdr-y))
all: $(addprefix $(out)/,$(syshdr-y))
	@:
