#!/bin/sh

. "$(dirname $0)/common.sh"

# Try to turn off a function that won't turn off.

FRAG=$(writefrag) << EOF
# CONFIG_MMU is not set
EOF

merge ${FRAG}
M=$?

# Return pass if MMU is still set in output

check CONFIG_MMU=y
G=$?

[ $M -ne 0 -a $G -eq 0 ]
