#!/bin/bash

help() {
	cat << EOF_HELP
Userspace compile test for exported kernel headers.

    Copyright (C) 2015 Mikko Rapeli <mikko.rapeli@iki.fi>

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; version 2
    of the License.

Execute in root directory of exported kernel headers in Linux kernel source
tree. Sets up gcc and libc headers without existing kernel headers to
a temporary environment and tries to compile all exported header files
from current directory against them. Return value is zero if all tests pass,
non-zero if something goes wrong during execution, or the amount of files
which failed the compile test.

Supported arguments:

    -h|--help       print help
    -k|--keep       don't cleanup temporary header files and directories
    -v|--verbose    print more verbose output

Example in Linux kernel source tree:

    \$ make headers_install
    \$ cd usr/include
    \$ $( readlink -f "$0" )

EOF_HELP
}

# bash due to arithmetics and pipefail
set -euo pipefail

KEEP=0
HELP=0

# command line arguments
for p in "$@"; do
	case "$p" in
		-k|--keep)
			KEEP=1
		;;
		-h|--help)
			HELP=1
		;;
		-v|--verbose)
			set -x
		;;
		*)
			help
			echo "Unknown argument: $p"
			exit 1
		;;
	esac
done

if [ "$HELP" != "0" ]; then help; exit 0; fi

# sanity test
if [ ! -d ./linux ]; then
	echo Sanity check error: ./linux directory not found
	echo Should be called in usr/include after \'make headers_install\'.
	echo Returns number of failed files, 0 if none.
	exit 1
fi

# Support CC variable for compiler and ccache, and cross compiling.
# CC is used without quotes to support CC="ccache gcc".
set +u
if [ "$CC"foobar == "foobar" ]; then
	CC=cc
fi

if [ "$CROSS_COMPILE"foobar != "foobar" ]; then
	# Using gcc name since some cross compiler tool chains don't provide
	# the cc symlink
	CC="$CROSS_COMPILE"gcc
fi
set -u

# Kernel headers refer to some gcc and libc headers so make them available.
set +u
if [ "$ARCH_TRIPLET"foobar == "foobar" ]; then
	# Taking triplet from gcc/cpp
	ARCH_TRIPLET="$( $CC -v -x c -E - < /dev/null 2>&1 | \
			grep Target | sed -e 's/Target: //' )"
fi

if [ "$LIBC"foobar == "foobar" ]; then
	# trying to grep libc includes from gcc/cpp defaults
	_TEMP="$( $CC -v -x c -E - < /dev/null 2>&1 | \
		sed -n -e '/^#include <...> search starts here:$/,/^End of search list.$/{//!p}' | \
		sed -e 's/^\ \//\//g' | \
		grep '/usr/include' )"

	# sanity check and prepare LIBC dirs
	for d in $_TEMP; do
		if [ ! -d "$d" ]; then
			echo "$d not a directory"
			exit 1
		fi
		LIBC="$LIBC $d"
	done
fi
set -u

# Copy libc include files to temp directory for the tests.
COMPILE_TEST_INC="$( readlink -f \
		"$( mktemp -d ../headers_compile_test_include.XXXXXX )" )"

# cleanup if keep not set
if [ "$KEEP" = "0" ]; then
	trap 'rm -rf "$COMPILE_TEST_INC"' EXIT
else
	trap 'printf \
"Temporary directory not cleaned up! Remove manually:\n${COMPILE_TEST_INC}\n"' \
		EXIT
fi

for d in $LIBC; do
	# check if last part of dir is the arch triplet, cross compile paths
	# can have it also elsewhere so just the last one counts.
	if ! ( echo "$d" | egrep "$ARCH_TRIPLET$" > /dev/null ); then
		# hopefully just main libc dir, e.g. /usr/include,
		# follow symlinks from e.g. /usr/include/bits
		cp -aL "$d"/* "$COMPILE_TEST_INC"/
	elif ( echo "$d" | egrep "$ARCH_TRIPLET$" > /dev/null ); then
		# hopefully the arch specific dir, e.g. /usr/include/x86_64-linux-gnu
		cp -ar "$d"/* "$COMPILE_TEST_INC/"
	else
		echo "$d unexpected, bailing out"
		exit 1
	fi
done

# Simulate libc headers without kernel headers by removing
# all known kernel header dirs from the copied libc ones.
# This seems to magically work.
_KERNEL_DIRS="$( find . -type d | grep -v '^\.$' )"
( cd "$COMPILE_TEST_INC" && rm -rf $_KERNEL_DIRS )

# GCC headers
set +u
if [ "$GCC_INC"foobar == "foobar" ]; then
	# Take from $CC default system include paths, filter out
	# /usr/local/include and /usr/include stuff first, then try to match
	# for gcc.
	_TEMP="$( $CC -v -x c -E - < /dev/null 2>&1 | \
		sed -n -e '/^#include <...> search starts here:$/,/^End of search list.$/{//!p}' | \
		sed -e 's/^\ \//\//g' | \
		egrep -v '/usr/local/include' | \
		egrep -v '/usr/include' | \
		grep gcc | \
		xargs )"

	# merge and prepare for use with $CC
	for d in $_TEMP; do
		# sanity test
		if [ ! -d "$d" ]; then
			echo "$d: is not a directory"
			exit 1
		fi
		GCC_INC="$GCC_INC -I $d"
	done
fi
set -u

# For each header file, try to compile it using the headers we prepared.
_FAILED=0
_PASSED=0
for f in $( find . -name "*\.h" | xargs ); do
	_FAIL=0

	# compile test, CC not quoted to support ccache
	echo $CC -Wall -c -nostdinc $GCC_INC -I . -I "$COMPILE_TEST_INC" -I "$COMPILE_TEST_INC/$ARCH_TRIPLET" -o /dev/null "$f"
	$CC -Wall -c -nostdinc $GCC_INC -I . -I "$COMPILE_TEST_INC" -I "$COMPILE_TEST_INC/$ARCH_TRIPLET" -o /dev/null "$f" \
		|| _FAIL=1

	# report errors
	if [ "$_FAIL" -gt 0 ]; then
		echo "FAILED: $f"
		_FAILED="$(( _FAILED + 1 ))"
	else
		echo "PASSED: $f"
		_PASSED="$(( _PASSED + 1))"
	fi
done

echo Statistics:
echo "$_FAILED files failed the compile test."
echo "$_PASSED files passed the compile test."

exit "$_FAILED"
