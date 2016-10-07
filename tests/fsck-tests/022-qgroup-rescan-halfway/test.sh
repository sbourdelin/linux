#!/bin/bash
# check whether btrfsck can detect running qgroup rescan

source $TOP/tests/common

check_prereq btrfs

for img in $(find . \( -iname '*.img' -o	\
			-iname '*.img.xz' -o 	\
			-iname '*.raw' -o 	\
			-iname '*.raw.xz' \) | sort)
do
	image=$(extract_image $img)
	run_check_stdout $TOP/btrfs check "$image" 2>&1 |\
		grep -q "Counts for qgroup id"
	if [ $? -eq 0 ]; then
		rm -f "$image"
		_fail "Btrfsck doesn't detect rescan correctly"
	fi
	rm -f "$image"
done
