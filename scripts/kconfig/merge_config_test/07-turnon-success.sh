#!/bin/sh

. "$(dirname $0)/common.sh"

# Try to turn on a function that will turn on.

FRAG=$(writefrag) << EOF
CONFIG_EMBEDDED=y
EOF

merge "${FRAG}"
M=$?

# Return fail if EMBEDDED is not set in output

check CONFIG_EMBEDDED=y
G=$?

[ $M -eq 0 -a $G -eq 0 ]
