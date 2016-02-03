#
# This file is subject to the terms and conditions of the GNU General Public
# License.  See the file "COPYING" in the main directory of this archive
# for more details.
#
# Copyright (C) 1995, 1998, 2001, 2002 by Ralf Baechle
# Copyright (C) 2004  Maciej W. Rozycki
#

#
# Some DECstations need all possible sections of an ECOFF executable
#
ifdef CONFIG_MACH_DECSTATION
  e2eflag := -a
endif

#
# Drop some uninteresting sections in the kernel.
# This is only relevant for ELF kernels but doesn't hurt a.out
#
drop-sections := .reginfo .mdebug .comment .note .pdr .options .MIPS.options
strip-flags   := $(addprefix --remove-section=,$(drop-sections))

hostprogs-y := elf2ecoff

suffix-y			:= bin
suffix-$(CONFIG_KERNEL_BZIP2)	:= bz2
suffix-$(CONFIG_KERNEL_GZIP)	:= gz
suffix-$(CONFIG_KERNEL_LZMA)	:= lzma
suffix-$(CONFIG_KERNEL_LZO)	:= lzo

targets := vmlinux.ecoff
quiet_cmd_ecoff = ECOFF	  $@
      cmd_ecoff = $(obj)/elf2ecoff $(VMLINUX) $@ $(e2eflag)
$(obj)/vmlinux.ecoff: $(obj)/elf2ecoff $(VMLINUX) FORCE
	$(call if_changed,ecoff)

targets += vmlinux.bin
quiet_cmd_bin = OBJCOPY $@
      cmd_bin = $(OBJCOPY) -O binary $(strip-flags) $(VMLINUX) $@
$(obj)/vmlinux.bin: $(VMLINUX) FORCE
	$(call if_changed,bin)

targets += vmlinux.srec
quiet_cmd_srec = OBJCOPY $@
      cmd_srec = $(OBJCOPY) -S -O srec $(strip-flags) $(VMLINUX) $@
$(obj)/vmlinux.srec: $(VMLINUX) FORCE
	$(call if_changed,srec)

UIMAGE_LOADADDR  = $(VMLINUX_LOAD_ADDRESS)
UIMAGE_ENTRYADDR = $(VMLINUX_ENTRY_ADDRESS)

#
# Compressed vmlinux images
#

extra-y += vmlinux.bin.bz2
extra-y += vmlinux.bin.gz
extra-y += vmlinux.bin.lzma
extra-y += vmlinux.bin.lzo

$(obj)/vmlinux.bin.bz2: $(obj)/vmlinux.bin FORCE
	$(call if_changed,bzip2)

$(obj)/vmlinux.bin.gz: $(obj)/vmlinux.bin FORCE
	$(call if_changed,gzip)

$(obj)/vmlinux.bin.lzma: $(obj)/vmlinux.bin FORCE
	$(call if_changed,lzma)

$(obj)/vmlinux.bin.lzo: $(obj)/vmlinux.bin FORCE
	$(call if_changed,lzo)

#
# Compressed u-boot images
#

targets += uImage
targets += uImage.bin
targets += uImage.bz2
targets += uImage.gz
targets += uImage.lzma
targets += uImage.lzo

$(obj)/uImage.bin: $(obj)/vmlinux.bin FORCE
	$(call if_changed,uimage,none)

$(obj)/uImage.bz2: $(obj)/vmlinux.bin.bz2 FORCE
	$(call if_changed,uimage,bzip2)

$(obj)/uImage.gz: $(obj)/vmlinux.bin.gz FORCE
	$(call if_changed,uimage,gzip)

$(obj)/uImage.lzma: $(obj)/vmlinux.bin.lzma FORCE
	$(call if_changed,uimage,lzma)

$(obj)/uImage.lzo: $(obj)/vmlinux.bin.lzo FORCE
	$(call if_changed,uimage,lzo)

$(obj)/uImage: $(obj)/uImage.$(suffix-y)
	@ln -sf $(notdir $<) $@
	@echo '  Image $@ is ready'

#
# Flattened Image Tree (.itb) images
#

targets += vmlinux.itb
targets += vmlinux.gz.itb

# Split a 64bit address into 2x32bit hex integers
its_addr = $(shell echo $(1) | sed 's/0x\(........\)\(........\)/0x\1 0x\2/')

its_load_addr := $(call its_addr,$(VMLINUX_LOAD_ADDRESS))
its_entry_addr := $(call its_addr,$(VMLINUX_ENTRY_ADDRESS))

its_files := $(patsubst %,$(srctree)/arch/mips/%,$(PLATFORM_ITS))
its_inc := $(patsubst %,/include/ \"%\"\n,$(PLATFORM_ITS))

quiet_cmd_gen-its = ITS     $@
      cmd_gen-its = sed "s|__VMLINUX__|$(2)|g" $< | \
		sed "s|__VMLINUX_COMPRESSION__|$(3)|g" | \
		sed "s|__PLATFORM_INCLUDES__|$(its_inc)|g" | \
		sed "s|__LOAD_ADDR__|$(its_load_addr)|g" | \
		sed "s|__ENTRY_ADDR__|$(its_entry_addr)|g" \
		>$@

quiet_cmd_itb-image = ITB     $@
      cmd_itb-image = \
		env PATH="$(objtree)/scripts/dtc:$(PATH)" \
		$(CONFIG_SHELL) $(MKIMAGE) \
		-D "-I dts -O dtb -p 500 --include $(srctree)/arch/mips --include $(objtree)/arch/mips" \
		-f $(3) $@

$(obj)/vmlinux.its: $(srctree)/arch/mips/boot/skeleton.its $(its_files) FORCE
	$(call if_changed,gen-its,boot/vmlinux.bin,none)

$(obj)/vmlinux.itb: $(obj)/vmlinux.bin $(obj)/vmlinux.its FORCE
	$(call if_changed,itb-image,none,$(obj)/vmlinux.its)

$(obj)/vmlinux.bz2.its: $(srctree)/arch/mips/boot/skeleton.its $(its_files) FORCE
	$(call if_changed,gen-its,boot/vmlinux.bin.bz2,bzip2)

$(obj)/vmlinux.bz2.itb: $(obj)/vmlinux.bin.bz2 $(obj)/vmlinux.bz2.its FORCE
	$(call if_changed,itb-image,bzip2,$(obj)/vmlinux.bz2.its)

$(obj)/vmlinux.gz.its: $(srctree)/arch/mips/boot/skeleton.its $(its_files) FORCE
	$(call if_changed,gen-its,boot/vmlinux.bin.gz,gzip)

$(obj)/vmlinux.gz.itb: $(obj)/vmlinux.bin.gz $(obj)/vmlinux.gz.its FORCE
	$(call if_changed,itb-image,gzip,$(obj)/vmlinux.gz.its)

$(obj)/vmlinux.lzma.its: $(srctree)/arch/mips/boot/skeleton.its $(its_files) FORCE
	$(call if_changed,gen-its,boot/vmlinux.bin.lzma,lzmaip)

$(obj)/vmlinux.lzma.itb: $(obj)/vmlinux.bin.lzma $(obj)/vmlinux.lzma.its FORCE
	$(call if_changed,itb-image,lzma,$(obj)/vmlinux.lzma.its)

$(obj)/vmlinux.lzo.its: $(srctree)/arch/mips/boot/skeleton.its $(its_files) FORCE
	$(call if_changed,gen-its,boot/vmlinux.bin.lzo,lzoip)

$(obj)/vmlinux.lzo.itb: $(obj)/vmlinux.bin.lzo $(obj)/vmlinux.lzo.its FORCE
	$(call if_changed,itb-image,lzo,$(obj)/vmlinux.lzo.its)
