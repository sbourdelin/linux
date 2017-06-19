#!/bin/sh
# Checks fast/slow prime_number generation for inconsistencies

if ! find /lib/modules/$(uname -r) -type f -name prime_numbers.ko | grep -q .;
then
	echo "prime_numbers: prime_numbers.ko not found: [SKIP]"
	exit 77
fi

if /sbin/modprobe -q prime_numbers selftest=65536; then
	/sbin/modprobe -q -r prime_numbers
	echo "prime_numbers: ok"
else
	echo "prime_numbers: [FAIL]"
	exit 1
fi
