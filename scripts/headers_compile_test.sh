#!/bin/bash
# bash due to arithmetics and pipefail
set -euo pipefail

# Debugging
#set -x

echo Simple compile test for header files meant for userspace.

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

COMPILE_TEST_INC=../headers_compile_test_include
rm -rf "$COMPILE_TEST_INC"
mkdir -p "$COMPILE_TEST_INC"

for d in $LIBC; do
	# check if last part of dir is the arch triplet, cross compile paths
	# can have it also elsewhere so just the last one counts.
	if !( echo "$d" | egrep "$ARCH_TRIPLET$" > /dev/null ); then
		# hopefully just main libc dir, e.g. /usr/include
		cp -a "$d"/* "$COMPILE_TEST_INC"/
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

# For each header file, create a .c which includes the header file
# and try to compile it using only current directory for searching other
# included header files.
_FAILED=0
_PASSED=0
for f in $( find . -name "*\.h" | xargs ); do
	_FAIL=0
	CFILE="$( echo "$( dirname "$f" )"/"$( basename "$f" .h )".c )"

	# create .c file
	echo "#include <"$( echo "$f" | sed -e 's|^.\/||' )">" \
		> "$(dirname "$f")"/"$(basename "$f" .h)".c

	# compile test, CC not quoted to support ccache
	echo $CC -Wall -c -nostdinc $GCC_INC -I . -I "$COMPILE_TEST_INC" -I "$COMPILE_TEST_INC/$ARCH_TRIPLET" "$CFILE"
	$CC -Wall -c -nostdinc $GCC_INC -I . -I "$COMPILE_TEST_INC" -I "$COMPILE_TEST_INC/$ARCH_TRIPLET" "$CFILE" \
		|| _FAIL=1

	# report errors
	if [ "$_FAIL" -gt 0 ]; then
		echo "FAILED: $f"
		_FAILED="$(( $_FAILED + 1 ))"
	else
		echo "PASSED: $f"
		_PASSED="$(( $_PASSED + 1))"
	fi
done

echo Statistics:
echo "$_FAILED files failed the compile test."
echo "$_PASSED files passed the compile test."

exit "$_FAILED"
