#!/bin/sh

. "$(dirname $0)/common.sh"

# Turn on an option that is already on

FRAG=$(writefrag) << EOF
CONFIG_MMU=y
EOF

merge "${FRAG}"
M=$?

# Return pass if MMU is still set in output

check CONFIG_MMU=y
G=$?

[ $M -eq 0 -a $G -eq 0 ]
