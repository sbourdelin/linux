#!/bin/bash
# This is a basic script to verify --init-csum recreates the csum tree.

source $TOP/tests/common

check_prereq btrfs-corrupt-block
check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev 512M


# simulate missing csum error and repair using init-csum option
test_csum_corruption()
{
	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f $TEST_DEV

	run_check_mount_test_dev

	export DATASET_SIZE=1
	generate_dataset small

	run_check_umount_test_dev

	# find bytenr
	bytenr=`$SUDO_HELPER $TOP/btrfs inspect-internal dump-tree -t csum $TEST_DEV | \
        sed -n -r 's/.*EXTENT_CSUM (.*)\) itemoff .*/\1/p'`

	# corrupt csum bytenr
	run_check $SUDO_HELPER $TOP/btrfs-corrupt-block -C $bytenr $TEST_DEV
	$SUDO_HELPER $TOP/btrfs check $TEST_DEV &>> $RESULTS && \
			_fail "btrfs check failed to detect missing csum."
	run_check $SUDO_HELPER $TOP/btrfs check --repair --init-csum $TEST_DEV
	run_check $SUDO_HELPER $TOP/btrfs check  $TEST_DEV
}

test_csum_corruption
