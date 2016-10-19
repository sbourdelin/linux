#!/bin/sh

# Copyright © 2016 IBM Corporation

# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version
# 2 of the License, or (at your option) any later version.

# This script checks the head of a vmlinux for linker stubs that
# break our placement of fixed-location code for 64-bit.

# based on relocs_check.pl
# Copyright © 2009 IBM Corporation

# READ THIS
#
# If the build dies here, it's likely code in head_64.S or nearby is
# referencing labels it can't reach, which results in the linker inserting
# stubs without the assembler's knowledge. This can move code around in ways
# that break the fixed location placement stuff (head-64.h). To debug,
# disassemble the vmlinux and look for branch stubs (long_branch, plt_branch
# etc) in the fixed section region (0 - 0x8000ish). Check what places are
# calling those stubs.
#
# Linker stubs use the TOC pointer, so even if fixed section code could
# tolerate them being inserted into head code, they can't be allowed in low
# level entry code (boot, interrupt vectors, etc) until r2 is set up. This
# could cause the kernel to die in early boot.

if [ $# -lt 2 ]; then
	echo "$0 [path to nm] [path to vmlinux]" 1>&2
	exit 1
fi

# Have Kbuild supply the path to nm so we handle cross compilation.
nm="$1"
vmlinux="$2"

nm "$vmlinux" | grep -e " T _stext$" -e " t start_first_256B$" -e " a text_start$" -e " t start_text$" -m4 > .tmp_symbols.txt


vma=$(cat .tmp_symbols.txt | grep " T _stext$" | cut -d' ' -f1)

expected_start_head_addr=$vma

start_head_addr=$(cat .tmp_symbols.txt | grep " t start_first_256B$" | cut -d' ' -f1)

if [ "$start_head_addr" != "$expected_start_head_addr" ]; then
	echo "ERROR: head code starts at $start_head_addr, should be 0"
	echo "ERROR: see comments in arch/powerpc/tools/head_check.sh"

	exit 1
fi

top_vma=$(echo $vma | cut -d'0' -f1)

expected_start_text_addr=$(cat .tmp_symbols.txt | grep " a text_start$" | cut -d' ' -f1 | sed "s/^0/$top_vma/")

start_text_addr=$(cat .tmp_symbols.txt | grep " t start_text$" | cut -d' ' -f1)

if [ "$start_text_addr" != "$expected_start_text_addr" ]; then
	echo "ERROR: start_text address is $start_text_addr, should be $expected_start_text_addr"
	echo "ERROR: see comments in arch/powerpc/tools/head_check.sh"

	exit 1
fi

rm .tmp_symbols.txt
