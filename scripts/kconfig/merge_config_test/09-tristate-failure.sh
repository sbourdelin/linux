#!/bin/sh

. "$(dirname $0)/common.sh"

# Try to turn on a tristate that failes

FRAG=$(writefrag) << EOF
CONFIG_MODULES=n
CONFIG_PCI_STUB=m
EOF

merge "${FRAG}"
M=$?

# Return fail if PCI_STUB=m is set in output

check CONFIG_PCI_STUB=m
G=$?

[ $M -ne 0 -a $G -ne 0 ]
