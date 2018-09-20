#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

in="$1"
out="$2"
my_abi="$3"

emit() {
    nxt="$1"
    nr="$2"
    entry="$3"

    while [ $nxt -lt $nr ]; do
	echo "__SYSCALL($nxt, sys_ni_syscall, )"
	let nxt=nxt+1
    done

    echo "__SYSCALL($nr, $entry, )"
}

grep '^[0-9]' "$in" | sort -n | (
    nxt=0
    while read nr abi name entry ; do
	emit $nxt $nr $entry
	nxt=$nr
        let nxt=nxt+1
    done
) > "$out"
