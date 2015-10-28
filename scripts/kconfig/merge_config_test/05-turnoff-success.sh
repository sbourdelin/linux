#!/bin/sh

. "$(dirname $0)/common.sh"

# Try to turn off a function that will turn off.

FRAG=$(writefrag) << EOF
# CONFIG_64BIT is not set
EOF

merge "${FRAG}"
M=$?

# Return fail if 64BIT is still set in output

check CONFIG_64BIT=y
G=$?

[ $M -eq 0 -a $G -ne 0 ]
