#!/bin/bash

source $TOP/tests/common

check_prereq btrfs-corrupt-block
check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev 512M

# verify that 'btrfs check --repair' fixes corrupted inode nlink field.
test_inode_nlink_field()
{
	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f $TEST_DEV

	run_check_mount_test_dev
	run_check $SUDO_HELPER touch $TEST_MNT/test_nlink.txt

	# find inode_item id
	inode_item=`stat -c%i $TEST_MNT/test_nlink.txt`
	run_check_umount_test_dev

	# corrupt nlink field of inode object
        run_check $SUDO_HELPER $TOP/btrfs-corrupt-block -i $inode_item \
		-f nlink $TEST_DEV

	$SUDO_HELPER $TOP/btrfs check $TEST_DEV &>> $RESULTS && \
			_fail "btrfs check failed to detect nlink corruption"
	run_check $SUDO_HELPER $TOP/btrfs check --repair $TEST_DEV
	run_check $SUDO_HELPER $TOP/btrfs check $TEST_DEV
}

test_inode_nlink_field
