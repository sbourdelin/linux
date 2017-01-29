#!/bin/bash
#
# convert non-raid btrfs to raid

source $TOP/tests/common

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev 1g
run_check truncate -s1g img

run_check $TOP/mkfs.btrfs -f $IMAGE
run_check_mount_test_dev

loopdev=$(run_check_stdout $SUDO_HELPER losetup --partscan --find --show img)

run_check $SUDO_HELPER $TOP/btrfs device add $loopdev $TEST_MNT
run_check $SUDO_HELPER $TOP/btrfs balance start -dconvert=raid1 -mconvert=raid1 $TEST_MNT

run_check_stdout $SUDO_HELPER $TOP/btrfs filesystem show $loopdev | grep "Total devices 2" -q
if [ $? -ne 0 ]; then
	run_check $SUDO_HELPER losetup -d $loopdev
	rm -f img
        _fail "Conversion from non-raid filesystem to raid failed."
fi
run_check_umount_test_dev

# cleanup
run_check $SUDO_HELPER losetup -d $loopdev
rm -f img
