#!/bin/sh
# Test whether the compile option -mprofile-kernel
# generates profiling code ( = a call to mcount), and
# whether a function without any global references sets
# the TOC pointer properly at the beginning, and
# whether the "notrace" function attribute successfully
# suppresses the _mcount call.

echo "int func() { return 0; }" | \
    $* -S -x c -O2 -p -mprofile-kernel - -o - 2> /dev/null | \
    grep -q "mcount"

trace_result=$?

echo "int func() { return 0; }" | \
    $* -S -x c -O2 -p -mprofile-kernel - -o - 2> /dev/null | \
    sed -n -e '/func:/,/bl _mcount/p' | grep -q TOC

leaf_toc_result=$?

/bin/echo -e "#include <linux/compiler.h>\nnotrace int func() { return 0; }" | \
    $* -S -x c -O2 -p -mprofile-kernel - -o - 2> /dev/null | \
    grep -q "mcount"

notrace_result=$?

if [ "$trace_result" -eq "0" -a \
	"$leaf_toc_result" -eq "0" -a \
	"$notrace_result" -eq "1" ]; then
	echo y
else
	echo n
fi
