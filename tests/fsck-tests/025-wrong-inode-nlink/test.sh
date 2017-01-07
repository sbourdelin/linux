#!/bin/bash

source $TOP/tests/common

check_prereq btrfs-corrupt-block
check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev 512M

# test whether fsck can fix a corrupted inode nlink
test_inode_nlink_field()
{
	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f $TEST_DEV

	run_check_mount_test_dev
	run_check $SUDO_HELPER touch $TEST_MNT/test_nlink.txt

	run_check_umount_test_dev

	# find inode_item id
	inode_item=`$SUDO_HELPER $TOP/btrfs-debug-tree -t FS_TREE $TEST_DEV | \
	grep -B3 "test_nlink.txt" |  grep INODE_ITEM | \
	cut -f2 -d'(' | cut -f1 -d' ' | head -n1`

	# corrupt nlink field of inode object:257
        run_check $SUDO_HELPER $TOP/btrfs-corrupt-block -i $inode_item \
		-f nlink $TEST_DEV

	$SUDO_HELPER $TOP/btrfs check $TEST_DEV >& /dev/null && \
			_fail "btrfs check failed to detect nlink corruption"
	run_check $SUDO_HELPER $TOP/btrfs check --repair $TEST_DEV
	run_check $SUDO_HELPER $TOP/btrfs check $TEST_DEV
}

test_inode_nlink_field
