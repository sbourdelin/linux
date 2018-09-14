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
    while read nr abi name entry compat ; do
	if [ "$my_abi" = "64" ]; then
            emit $nxt $nr $entry
	elif [ "$my_abi" = "32" ]; then
            emit $nxt $nr $entry
	elif [ "$my_abi" = "c32" ]; then
	    if [ -z "$compat" ]; then
		emit $nxt $nr $entry
	    else
		emit $nxt $nr $compat
	    fi
	fi
	nxt=$nr
        let nxt=nxt+1
    done
) > "$out"
