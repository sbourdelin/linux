#!/bin/sh
set -e

vmlinuz="$1"
zimage="$2"
nm="$3"

magic_size=$("$nm" "$vmlinuz" | perl -e 'while (<>) {
	$magic_start = hex($1) if /^([[:xdigit:]]+) . _magic_start$/;
	$magic_end = hex($1) if /^([[:xdigit:]]+) . _magic_end$/;
}; printf "%d\n", $magic_end - $magic_start;')

zimage_size=$(stat -c '%s' "$zimage")

# Verify that the resulting binary matches the size contained within
# the binary (iow, the linker has not added any additional sections.)
if [ $magic_size -ne $zimage_size ]; then
   echo "zImage size ($zimage_size) disagrees with linked size ($magic_size)" >&2
   exit 1
fi
exit 0
