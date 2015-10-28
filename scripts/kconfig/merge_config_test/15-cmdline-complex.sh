#!/bin/sh

. "$(dirname $0)/common.sh"

# Make sure redundant options are warned about

FRAG1=$(writefrag) << EOF
CONFIG_EMBEDDED=y
EOF

FRAG2=$(writefrag) << EOF
CONFIG_MMU=n
EOF

merge_r ${FRAG1} CONFIG_64BIT=n ${FRAG2}
M=$?

check CONFIG_64BIT=y
G1=$?

check CONFIG_EMBEDDED=y
G2=$?

check CONFIG_MMU=y
G3=$?

[ $G1 -ne 0 -a $G2 -eq 0 -a $G3 -eq 0 ]
G=$?

[ $M -ne 0 -a $G -eq 0 ]
