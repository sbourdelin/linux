#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

in="$1"
out="$2"
my_abis=`echo "($3)" | tr ',' '|'`
offset="$4"

emit() {
    nxt="$1"
    if [ -z "$offset" ]; then
	nr="$2"
    else
	nr="$2"
	nr=$((nr+offset))
    fi
    entry="$3"

    while [ $nxt -lt $nr ]; do
	echo "__SYSCALL($nxt, sys_ni_syscall, )"
        let nxt=nxt+1
    done
    echo "__SYSCALL($nxt, $entry, )"
}

grep -E "^[0-9A-Fa-fXx]+[[:space:]]+${my_abis}" "$in" | sort -n | (
    if [ -z "$offset" ]; then
	nxt=0
    else
	nxt=$offset
    fi

    while read nr abi name entry ; do
	emit $nxt $nr $entry
	let nxt=nxt+1
    done
) > "$out"
