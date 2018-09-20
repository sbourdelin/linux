#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

in="$1"
out="$2"
my_abis=`echo "($3)" | tr ',' '|'`
prefix="$4"
offset="$5"

fileguard=_UAPI_ASM_M68K_`basename "$out" | sed \
    -e 'y/abcdefghijklmnopqrstuvwxyz/ABCDEFGHIJKLMNOPQRSTUVWXYZ/' \
    -e 's/[^A-Z0-9_]/_/g' -e 's/__/_/g'`_
grep -E "^[0-9A-Fa-fXx]+[[:space:]]+${my_abis}" "$in" | sort -n | (
    echo "#ifndef ${fileguard}"
    echo "#define ${fileguard}"
    echo ""

    nxt=0
    while read nr abi name entry ; do
	if [ -z "$offset" ]; then
	    echo -e "#define __NR_${prefix}${name}\t$nr"
	else
	    echo -e "#define __NR_${prefix}${name}\t($offset + $nr)"
	fi
	nxt=$nr
	let nxt=nxt+1
    done

    echo ""
    echo "#ifdef __KERNEL__"
    if [ -z "$offset" ]; then
        echo -e "#define __NR_syscalls\t$nxt"
    else
        echo -e "#define __NR_syscalls\t($offset + $nxt)"
    fi
    echo "#endif"
    echo ""
    echo "#endif /* ${fileguard} */"
) > "$out"
