#!/bin/sh

. "$(dirname $0)/common.sh"

# Try to turn on a tristate that is allowed

FRAG=$(writefrag) << EOF
CONFIG_MODULES=y
CONFIG_PCI_STUB=m
EOF

merge "${FRAG}"
M=$?

# Return fail if PCI_STUB=m is not set in output

check CONFIG_PCI_STUB=m
G=$?

[ $M -eq 0 -a $G -eq 0 ]
