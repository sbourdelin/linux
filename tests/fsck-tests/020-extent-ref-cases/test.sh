#!/bin/bash

source $TOP/tests/common

check_prereq btrfs

for img in *.img
do
	image=$(extract_image $img)
	run_check_stdout $TOP/btrfs check "$image" 2>&1 |
		grep -q "Errors found in extent allocation tree or chunk allocation" &&
		_fail "unexpected error occurred when checking $img"
	rm -f "$image"
done
