#!/bin/bash
#
# Corrupt primary superblock and restore it using backup superblock.
#

source $TOP/tests/common

check_prereq btrfs-select-super
check_prereq btrfs

setup_root_helper
prepare_test_dev 512M

FIRST_SUPERBLOCK_OFFSET=65536

test_superblock_restore()
{
	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f $TEST_DEV

	# Corrupt superblock checksum
        dd if=/dev/zero of=$TEST_DEV seek=$FIRST_SUPERBLOCK_OFFSET bs=1 \
        count=4  conv=notrunc &> /dev/null
	
	# Run btrfs check to detect corruption
	$TOP/btrfs check $TEST_DEV >& /dev/null && \
		_fail "btrfs check should detect corruption"

	# Copy backup superblock to primary
	run_check $TOP/btrfs-select-super -s 1 $TEST_DEV
	
	echo "Performing btrfs check" &>> $RESULTS
	$TOP/btrfs check $TEST_DEV &>> $RESULTS
        if [ $? -ne 0 ]; then
		_fail "Failed to fix superblock."
        fi 
}

test_superblock_restore
