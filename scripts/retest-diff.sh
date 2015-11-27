#!/bin/bash
#
# Copyright © 2015 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

ROOT="`dirname $0`"
ROOT="`readlink -f $ROOT/..`"
IGT_TEST_ROOT="$ROOT/tests"
RESULTS="$ROOT/results"
PIGLIT="$ROOT/piglit/piglit"

if [ ! -d "$IGT_TEST_ROOT" ]; then
	echo "Error: could not find tests directory."
	exit 1
fi

function print_help {
	echo "Usage: $0 [<Results Paths> ...]"
	echo ""
}


if [ $# -lt 2 ]; then
	print_help
	exit 1
fi

RESULT_FILES=$@

for result in $RESULT_FILES; do
    if [ ! -e $result ]; then
        echo "Wrong result paths"
        exit 1
    fi
done


TESTS=`$PIGLIT summary console -d $RESULT_FILES | grep igt | cut -d':' -f1`
if [[ -z $TESTS ]]; then
    exit 1
else
    RED='\033[0;31m'
    NC='\033[0m'
    printf "$RED $TESTS $NC " | sed -e 's/ /\n/g'
fi


read -p "Dow you want to run these tests [y/N]? " -n 2 -r
if [[ $REPLY =~ ^[Yy]$ ]]; then
    mkdir -p $RESULTS
    TEST_LIST=($TESTS)
    TEST_PARAMS=`printf "%s "  "${TEST_LIST[@]/#/-t }"`
    sudo IGT_TEST_ROOT=$IGT_TEST_ROOT $PIGLIT run igt $RESULTS $TEST_PARAMS
fi
