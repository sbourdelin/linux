#!/bin/bash
# check whether btrfsck quota verify will cause stack overflow.
# this is caused by lack of handle for tree reloc tree.
# fixed by patch:
# btrfs-progs: Fix stack overflow for checking qgroup on tree reloc tree

source $TOP/tests/common

check_prereq btrfs

for img in $(find . \( -iname '*.img' -o	\
			-iname '*.img.xz' -o 	\
			-iname '*.raw' -o 	\
			-iname '*.raw.xz' \) | sort)
do
	image=$(extract_image $img)
	run_check $TOP/btrfs check "$image"
	rm -f "$image"
done
