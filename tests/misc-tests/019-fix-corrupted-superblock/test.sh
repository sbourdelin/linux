#!/bin/bash
#
# Corrupt primary superblock and restore it using backup superblock.
#

source $TOP/tests/common

check_prereq btrfs-select-super

setup_root_helper
prepare_test_dev 512M

superblock_offset=65536

test_superblock_restore()
{
	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f $TEST_DEV

	# Corrupt superblock checksum
        dd if=/dev/zero of=$TEST_DEV seek=$superblock_offset bs=1 \
        count=4  conv=notrunc &> /dev/null
	run_check_stdout $SUDO_HELPER mount $TEST_DEV $TEST_MNT | \
	grep -q 'wrong fs type'
        if [ $? -ne 0 ]; then
		_fail "Failed to corrupt superblock."
        fi 

	# Copy backup superblock to primary
	run_check $TOP/btrfs-select-super -s 1 $TEST_DEV
	run_check $SUDO_HELPER mount $TEST_DEV $TEST_MNT
        if [ $? -ne 0 ]; then
		_fail "Failed to fix superblock."
        fi 
	run_check_umount_test_dev
}

test_superblock_restore
