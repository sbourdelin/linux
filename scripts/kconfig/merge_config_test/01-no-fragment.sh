#!/bin/sh

. "$(dirname $0)/common.sh"

# Simple merge: No fragment specified, just base config

FRAG=$(echo "" | writefrag)

merge ${FRAG}
M=$?

[ $M -eq 0 ]
