#!/bin/sh

. "$(dirname $0)/common.sh"

# Turn on something that's already on, watch it warn/fail

FRAG=$(writefrag) << EOF
CONFIG_64BIT=y
EOF

merge_r "${FRAG}" "${FRAG}"
M=$?

# Return fail if 64BIT=y is not set in output

check CONFIG_64BIT=y
G=$?

[ $M -ne 0 -a $G -eq 0 ]
