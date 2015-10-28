#!/bin/sh

EXITVAL=0
TMPDIR=$(mktemp -d /tmp/mergetest.XXXXX) || exit 1
ARCH=x86

cleanup() {
	rm -rf "${TMPDIR}"
	exit $EXITVAL
}

trap cleanup EXIT

for test in $(dirname $0)/*-*.sh ; do
	echo -n "test $(basename ${test}):  "
	if ${test} >/dev/null 2>&1 ; then
		echo PASSED
	else
		echo FAILED
		EXITVAL=1
	fi
done
