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
    nxt=4000
    while read nr abi name entry compat ; do
	if [ "$my_abi" = "32-o32" ]; then 
	    let t_nxt=$nxt+0
            emit $t_nxt $nr $entry
	elif [ "$my_abi" = "64-o32" ]; then
	    let t_nxt=$nxt+0
	    if [ -z "$compat" ]; then
		emit $t_nxt $nr $entry
	    else
		emit $t_nxt $nr $compat
	    fi
	elif [ "$my_abi" = "64-64" ]; then
	    let t_nxt=$nxt+1000
            emit $t_nxt $nr $entry
	elif [ "$my_abi" = "64-n32" ]; then
	    let t_nxt=$nxt+2000
            emit $t_nxt $nr $entry
	fi
	nxt=$nr
        let nxt=nxt+1
    done
) > "$out"
