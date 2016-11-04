#!/bin/bash
#
# test for sending stream size of clone-src option

source $TOP/tests/common

check_prereq mkfs.btrfs
check_prereq btrfs

setup_root_helper
prepare_test_dev 2g

run_check $TOP/mkfs.btrfs -f $IMAGE
run_check_mount_test_dev

here=`pwd`
cd "$TEST_MNT" || _fail "cannot chdir to TEST_MNT"

run_check $SUDO_HELPER btrfs subvolume create subv-parent1
run_check $SUDO_HELPER dd if=/dev/urandom of=subv-parent1/file1_1 bs=1M count=10
run_check $SUDO_HELPER btrfs subvolume snapshot -r subv-parent1 subv-snap1_1
run_check $SUDO_HELPER dd if=/dev/urandom of=subv-parent1/file1_2 bs=1M count=10
run_check $SUDO_HELPER btrfs subvolume snapshot -r subv-parent1 subv-snap1_2
run_check $SUDO_HELPER dd if=/dev/urandom of=subv-parent1/file1_3 bs=1M count=10
run_check $SUDO_HELPER btrfs subvolume snapshot -r subv-parent1 subv-snap1_3

run_check $SUDO_HELPER btrfs subvolume create subv-parent2
run_check $SUDO_HELPER dd if=/dev/urandom of=subv-parent2/file2_1 bs=1M count=10
run_check $SUDO_HELPER btrfs subvolume snapshot -r subv-parent2 subv-snap2_1
run_check $SUDO_HELPER dd if=/dev/urandom of=subv-parent2/file2_2 bs=1M count=10
run_check $SUDO_HELPER btrfs subvolume snapshot -r subv-parent2 subv-snap2_2
run_check $SUDO_HELPER dd if=/dev/urandom of=subv-parent2/file2_3 bs=1M count=10
run_check $SUDO_HELPER btrfs subvolume snapshot -r subv-parent2 subv-snap2_3

run_check $SUDO_HELPER btrfs send -f "$here"/send.stream.before \
	-c subv-snap1_1 -c subv-snap2_1 subv-snap1_[23] subv-snap2_[23]

run_check $SUDO_HELPER $TOP/btrfs send -f "$here"/send.stream.after \
	-c subv-snap1_1 -c subv-snap2_1 subv-snap1_[23] subv-snap2_[23]

before_size=`ls -l "$here"/send.stream.before | awk '{print $5}'`
after_size=`ls -l "$here"/send.stream.after | awk '{print $5}'`

if [ $before_size -lt $after_size ]; then
	run_check ls -l "$here"/send.stream.*
	_fail "sending stream size is bigger than old stream"
fi

run_check rm -f "$here"/send.stream.*

cd "$here" || _fail "cannot chdir back to test directory"

run_check_umount_test_dev

