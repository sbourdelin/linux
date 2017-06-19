#!/bin/sh
# Runs bitmap infrastructure tests using test_bitmap kernel module

if ! find /lib/modules/$(uname -r) -type f -name test_bitmap.ko | grep -q .;
then
	echo "bitmap: test_bitmap.ko not found [SKIP]"
	exit 77
fi

if /sbin/modprobe -q test_bitmap; then
	/sbin/modprobe -q -r test_bitmap
	echo "bitmap: ok"
else
	echo "bitmap: [FAIL]"
	exit 1
fi
