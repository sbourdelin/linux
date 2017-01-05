#!/bin/bash

source $TOP/tests/common

check_prereq btrfs-corrupt-block
check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev 512M

ROOT_NODE=256
BTRFS_DIR_ITEM_KEY=84

# test whether fsck can detect a corrupted dir item name
test_dir_item_name_field()
{
	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f $TEST_DEV

	run_check_mount_test_dev
	run_check $SUDO_HELPER touch $TEST_MNT/testfile.txt

	run_check_umount_test_dev

	# find key offset
	key_offset=`$SUDO_HELPER $TOP/btrfs-debug-tree -t FS_TREE $TEST_DEV | \
	grep -B3 'testfile.txt'  | grep "$ROOT_NODE DIR_ITEM" | \
	cut -f1 -d')' | awk '{print $6}'`

	key=$ROOT_NODE","$BTRFS_DIR_ITEM_KEY","$key_offset

	# corrupt dir item name
        run_check $SUDO_HELPER $TOP/btrfs-corrupt-block -D -f name \
		-K $key $TEST_DEV

	$SUDO_HELPER $TOP/btrfs check $TEST_DEV >& /dev/null && \
			_fail "btrfs check failed to detect corruption"
	run_check $SUDO_HELPER $TOP/btrfs check --repair $TEST_DEV
}

test_dir_item_name_field
