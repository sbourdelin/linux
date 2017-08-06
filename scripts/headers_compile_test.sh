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
non-zero if something goes wrong during execution or if any file failed
the compile tests.

Supported arguments:

    -h|--help          print help
    -k|--keep          don't cleanup temporary header files and directories
    -lk|--libc-kernel  test for conflicts between kernel and libc headers
                       when libc headers are included before kernel headers
    -kl|--kernel-libc  test for conflicts between kernel and libc headers
                       when kernel headers are included before libc headers
    -l|--libc          test both -lk and -kl
    -v|--verbose       print more verbose output

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
LIBC_TEST=0
LIBC_KERNEL_TEST=0
KERNEL_LIBC_TEST=0

# command line arguments
for p in "$@"; do
	case "$p" in
		-k|--keep)
			KEEP=1
		;;
		-l|--libc)
			LIBC_TEST=1
			LIBC_KERNEL_TEST=1
			KERNEL_LIBC_TEST=1
		;;
		-lk|--libc-kernel)
			LIBC_TEST=1
			LIBC_KERNEL_TEST=1
		;;
		-kl|--kernel-libc)
			LIBC_TEST=1
			KERNEL_LIBC_TEST=1
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
	# the cc symlink. Using eval to expand ~ to $HOME.
	CC="$( eval echo "$CROSS_COMPILE"gcc )"
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

# A single header with all non-conflicting libc headers to test kernel
# headers against libc headers for conflicts.
if [ "$LIBC_TEST" != 0 ]; then
	# List taken from Debian unstable libc6 version 2.21-9.
	# Some glibc headers conflict with each other so they
	# are filtered out. Not perfect but better than nothing.
        #
	# $ for f in $( egrep "\.h$" /var/lib/dpkg/info/libc6-dev\:i386.list | sed -e 's|/usr/include/||'| sort | grep -v arpa | grep -v linux-gnu | grep -v rpcsvc | grep -v regexp.h | grep -v rpc | grep -v scsi | grep -v talkd ); do echo "#include <$f>"; done > libc_headers.h

	cat > "$COMPILE_TEST_INC/libc_headers.h" << EOF_LIBC_HEADERS
#include <aio.h>
#include <aliases.h>
#include <alloca.h>
#include <argp.h>
#include <argz.h>
#include <ar.h>
#include <assert.h>
#include <byteswap.h>
#include <complex.h>
#include <cpio.h>
#include <crypt.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <endian.h>
#include <envz.h>
#include <err.h>
#include <errno.h>
#include <error.h>
#include <execinfo.h>
#include <fcntl.h>
#include <features.h>
#include <fenv.h>
#include <fmtmsg.h>
#include <fnmatch.h>
#include <fstab.h>
#include <fts.h>
#include <ftw.h>
#include <_G_config.h>
#include <gconv.h>
#include <getopt.h>
#include <glob.h>
#include <gnu-versions.h>
#include <grp.h>
#include <gshadow.h>
#include <iconv.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <langinfo.h>
#include <lastlog.h>
#include <libgen.h>
#include <libintl.h>
#include <libio.h>
#include <limits.h>
#include <link.h>
#include <locale.h>
#include <malloc.h>
#include <math.h>
#include <mcheck.h>
#include <memory.h>
#include <mntent.h>
#include <monetary.h>
#include <mqueue.h>
#include <netash/ash.h>
#include <netatalk/at.h>
#include <netax25/ax25.h>
#include <netdb.h>
#include <neteconet/ec.h>
#include <net/ethernet.h>
#include <net/if_arp.h>
#include <net/if.h>
#include <net/if_packet.h>
#include <net/if_ppp.h>
#include <net/if_shaper.h>
#include <net/if_slip.h>
#include <netinet/ether.h>
#include <netinet/icmp6.h>
#include <netinet/if_ether.h>
#include <netinet/if_fddi.h>
#include <netinet/if_tr.h>
#include <netinet/igmp.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip6.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netipx/ipx.h>
#include <netiucv/iucv.h>
#include <netpacket/packet.h>
#include <net/ppp-comp.h>
#include <net/ppp_defs.h>
#include <netrom/netrom.h>
#include <netrose/rose.h>
#include <net/route.h>
#include <nfs/nfs.h>
#include <nl_types.h>
#include <nss.h>
#include <obstack.h>
#include <paths.h>
#include <poll.h>
#include <printf.h>
#include <protocols/routed.h>
#include <protocols/rwhod.h>
#include <protocols/timed.h>
#include <pthread.h>
#include <pty.h>
#include <pwd.h>
#include <re_comp.h>
#include <regex.h>
#include <resolv.h>
#include <sched.h>
#include <search.h>
#include <semaphore.h>
#include <setjmp.h>
#include <sgtty.h>
#include <shadow.h>
#include <signal.h>
#include <spawn.h>
#include <stab.h>
#include <stdc-predef.h>
#include <stdint.h>
#include <stdio_ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stropts.h>
#include <syscall.h>
#include <sysexits.h>
#include <syslog.h>
#include <tar.h>
#include <termio.h>
#include <termios.h>
#include <tgmath.h>
#include <thread_db.h>
#include <time.h>
#include <ttyent.h>
#include <uchar.h>
#include <ucontext.h>
#include <ulimit.h>
#include <unistd.h>
#include <ustat.h>
#include <utime.h>
#include <utmp.h>
#include <utmpx.h>
#include <values.h>
#include <wait.h>
#include <wchar.h>
#include <wctype.h>
#include <wordexp.h>
#include <xlocale.h>
EOF_LIBC_HEADERS

fi # LIBC_TEST

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

# sanity check: test that plain libc headers compile
if [ "$LIBC_TEST" != 0 ]; then
	echo "Testing that $COMPILE_TEST_INC/libc_headers.h compiles"
	$CC -Wall -c -nostdinc $GCC_INC -I . \
		-I "$COMPILE_TEST_INC" \
		-I "$COMPILE_TEST_INC/$ARCH_TRIPLET" \
		-o /dev/null \
		"$COMPILE_TEST_INC/libc_headers.h"
fi

# Summary counters:
_FAILED=0
_PASSED=0
_LIBC_FAILED=0
_LIBC_PASSED=0
_LIBC_BEFORE_KERNEL_FAILED=0
_LIBC_BEFORE_KERNEL_PASSED=0
_KERNEL_BEFORE_LIBC_FAILED=0
_KERNEL_BEFORE_LIBC_PASSED=0

# For each header file, try to compile it using the headers we prepared.
for f in $( find . -name "*\.h" -printf "%P\n" ); do
	_FAIL=0
	_FAIL_LIBC=0
	_FAIL_LIBC_BEFORE_KERNEL=0
	_FAIL_KERNEL_BEFORE_LIBC=0

	# compile test, CC not quoted to support ccache
	echo $CC -Wall -c -nostdinc $GCC_INC -I . -I "$COMPILE_TEST_INC" -I "$COMPILE_TEST_INC/$ARCH_TRIPLET" -o /dev/null "$PWD/$f"
	$CC -Wall -c -nostdinc $GCC_INC -I . -I "$COMPILE_TEST_INC" -I "$COMPILE_TEST_INC/$ARCH_TRIPLET" -o /dev/null "$PWD/$f" \
		|| _FAIL=1

	# report errors
	if [ "$_FAIL" -gt 0 ]; then
		echo "FAILED: $f"
		_FAILED="$(( _FAILED + 1 ))"
	else
		echo "PASSED: $f"
		_PASSED="$(( _PASSED + 1))"
	fi

	# libc header conflict tests
	if [ "$LIBC_TEST" != 0 ]; then
		_LIBC_BEFORE_KERNEL="$PWD/$f"_libc_before_kernel.h
		_KERNEL_BEFORE_LIBC="$PWD/$f"_kernel_before_libc.h

		# libc header included before kernel header
		if [ "$LIBC_KERNEL_TEST" != 0 ]; then
			cat > "$_LIBC_BEFORE_KERNEL" << EOF_LIBC_BEFORE_KERNEL
#include <libc_headers.h>
#include <$f>
EOF_LIBC_BEFORE_KERNEL
			echo \
				$CC -Wall -c -nostdinc $GCC_INC \
				-I . -I "$COMPILE_TEST_INC" \
				-I "$COMPILE_TEST_INC/$ARCH_TRIPLET" \
				-o /dev/null "$_LIBC_BEFORE_KERNEL"
			$CC -Wall -c -nostdinc $GCC_INC \
				-I . -I "$COMPILE_TEST_INC" \
				-I "$COMPILE_TEST_INC/$ARCH_TRIPLET" \
				-o /dev/null "$_LIBC_BEFORE_KERNEL" \
				|| _FAIL_LIBC_BEFORE_KERNEL=1

			# report errors
			if [ "$_FAIL_LIBC_BEFORE_KERNEL" -gt 0 ]; then
				echo "FAILED libc before kernel test: $f"
				_LIBC_BEFORE_KERNEL_FAILED="$(( _LIBC_BEFORE_KERNEL_FAILED + 1 ))"
			else
				echo "PASSED libc before kernel test: $f"
				_LIBC_BEFORE_KERNEL_PASSED="$(( _LIBC_BEFORE_KERNEL_PASSED + 1))"
			fi
		fi

		# kernel header included before libc
		if [ "$KERNEL_LIBC_TEST" != 0 ]; then
			cat > "$_KERNEL_BEFORE_LIBC" << EOF_KERNEL_BEFORE_LIBC
#include <$f>
#include <libc_headers.h>
EOF_KERNEL_BEFORE_LIBC
			echo \
				$CC -Wall -c -nostdinc $GCC_INC \
				-I . -I "$COMPILE_TEST_INC" \
				-I "$COMPILE_TEST_INC/$ARCH_TRIPLET" \
				-o /dev/null "$_KERNEL_BEFORE_LIBC"
			$CC -Wall -c -nostdinc $GCC_INC \
				-I . -I "$COMPILE_TEST_INC" \
				-I "$COMPILE_TEST_INC/$ARCH_TRIPLET" \
				-o /dev/null "$_KERNEL_BEFORE_LIBC" \
				|| _FAIL_KERNEL_BEFORE_LIBC=1

			# report errors
			if [ "$_FAIL_KERNEL_BEFORE_LIBC" -gt 0 ]; then
				echo "FAILED kernel before libc test: $f"
				_KERNEL_BEFORE_LIBC_FAILED="$(( _KERNEL_BEFORE_LIBC_FAILED + 1 ))"
			else
				echo "PASSED kernel before libc test: $f"
				_KERNEL_BEFORE_LIBC_PASSED="$(( _KERNEL_BEFORE_LIBC_PASSED + 1))"
			fi
		fi

		# libc summary
		if [ "$_FAIL_LIBC_BEFORE_KERNEL" -gt 0 -o "$_FAIL_KERNEL_BEFORE_LIBC" -gt 0 ]; then
			_LIBC_FAILED="$(( _LIBC_FAILED + 1))"
		else
			_LIBC_PASSED="$(( _LIBC_PASSED + 1))"
		fi

		if [ "$KEEP" = "0" ]; then
			rm -f "$_LIBC_BEFORE_KERNEL" "$_KERNEL_BEFORE_LIBC"
		fi
	fi # LIBC_TEST
done

cat << EOF_STATS

Kernel header compile test statistics:

$_FAILED files failed the kernel header compile test.
$_PASSED files passed the kernel header compile test.

EOF_STATS

if [ "$LIBC_TEST" != 0 ]; then
	cat << EOF_LIBC_STATS
libc and kernel header compatibility test statistics:
EOF_LIBC_STATS

if [ "$LIBC_KERNEL_TEST" != 0 ] && [ "$KERNEL_LIBC_TEST" != 0 ]; then
	cat << EOF_LIBC_COMBINED
$_LIBC_FAILED files failed the libc compatibility test.
$_LIBC_PASSED files passed the libc compatibility test.
EOF_LIBC_COMBINED
fi

if [ "$LIBC_KERNEL_TEST" != 0 ]; then
	cat << EOF_LIBC_KERNEL
$_LIBC_BEFORE_KERNEL_FAILED files failed libc before kernel include test.
$_LIBC_BEFORE_KERNEL_PASSED files passed libc before kernel include test.
EOF_LIBC_KERNEL
fi

if [ "$KERNEL_LIBC_TEST" != 0 ]; then
	cat << EOF_KERNEL_LIBC
$_KERNEL_BEFORE_LIBC_FAILED files failed kernel before libc include test.
$_KERNEL_BEFORE_LIBC_PASSED files passed kernel before libc include test.
EOF_KERNEL_LIBC
fi

fi # LIBC_TEST

# return value, summary of all failures.
if [ "$(( $_FAILED + $_LIBC_BEFORE_KERNEL_FAILED + $_KERNEL_BEFORE_LIBC_FAILED ))" != 0 ]; then
	exit 1
else
	exit 0
fi
