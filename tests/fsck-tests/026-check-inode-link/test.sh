#!/bin/bash
# verify that 'btrfs check --repair' fixes corrupted inode nlink field

source $TOP/tests/common

check_prereq btrfs-corrupt-block
check_prereq mkfs.btrfs

setup_root_helper
prepare_test_dev 512M

test_inode_nlink_field()
{
	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f $TEST_DEV

	run_check_mount_test_dev
	run_check $SUDO_HELPER touch $TEST_MNT/test_nlink.txt

	# find inode_number
	inode_number=`stat -c%i $TEST_MNT/test_nlink.txt`
	run_check_umount_test_dev

	# corrupt nlink field of inode object
        run_check $SUDO_HELPER $TOP/btrfs-corrupt-block -i $inode_number \
		-f nlink $TEST_DEV

	check_image $TEST_DEV
}

test_inode_nlink_field
