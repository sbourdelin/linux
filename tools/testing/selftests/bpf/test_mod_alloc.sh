#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
UNMOUNT_DEBUG_FS=0
if ! mount | grep -q debugfs; then
	if mount -t debugfs none /sys/kernel/debug/; then
		UNMOUNT_DEBUG_FS=1
	else
		echo "Could not mount debug fs."
		exit 1
	fi
fi

if [ ! -e /sys/kernel/debug/mod_alloc_test ]; then
	echo "Test module not found, did you build kernel with TEST_MOD_ALLOC?"
	exit 1
fi

echo "Beginning module_alloc performance tests."

for i in `seq 1000 1000 8000`; do
	echo m$i>/sys/kernel/debug/mod_alloc_test
	echo t2>/sys/kernel/debug/mod_alloc_test
done

echo "Module_alloc performance tests ended."

if [ $UNMOUNT_DEBUG_FS -eq 1 ]; then
	umount /sys/kernel/debug/
fi
