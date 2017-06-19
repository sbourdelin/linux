#!/bin/sh
# Runs printf infrastructure using test_printf kernel module

if ! find /lib/modules/$(uname -r) -type f -name test_printf.ko | grep -q .;
then
        echo "printf: test_printf.ko not found [SKIP]"
        exit 77
fi

if /sbin/modprobe -q test_printf; then
	/sbin/modprobe -q -r test_printf
	echo "printf: ok"
else
	echo "printf: [FAIL]"
	exit 1
fi
