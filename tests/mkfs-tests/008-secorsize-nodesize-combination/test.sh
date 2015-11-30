#!/bin/bash
# test variant sectorsize and node size combination
# including valid ones and invalid ones
# only do mkfs and fsck check, no mounting check as
# sub/multi-pagesize is not supported yet.

source $TOP/tests/common

check_prereq mkfs.btrfs
check_prereq btrfs

# disable mixed bg to avoid sectorsize == nodesize check
features="^mixed-bg"
image_size=1g
image=image

# caller need to judge whether the combination is valid
do_test()
{
	sectorsize=$1
	nodesize=$2
	truncate -s $image_size $image
	run_mayfail $TOP/mkfs.btrfs -O $features -n $nodesize -s $sectorsize \
		$image
	ret=$?
	if [ $ret == 0 ]; then
		run_check $TOP/btrfs check $image
	fi
	return $ret
}

# Invalid: Unaligned sectorsize and nodesize
do_test 8191 8191 && _fail

# Invalid: Aligned sectorsize with unaligned nodesize
do_test 4k 16385 && _fail

# Invalid: Ungliend sectorsize with aligned nodesize
do_test 8191 16k && _fail

# Valid: Aligned sectorsize and nodesize
do_test 4k 16k || _fail

# Invalid: Sectorsize larger than nodesize
do_test 8k 4k && _fail

# Invalid: too large nodesize
do_test 16k 128k && _fail

# Valid: large sectorsize
do_test 64k 64k || _fail
