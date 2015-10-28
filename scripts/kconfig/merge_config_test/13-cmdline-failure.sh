#!/bin/sh

. "$(dirname $0)/common.sh"

# Try to turn off an option that won't turn off.

merge CONFIG_MMU=n
M=$?

check CONFIG_MMU=y
G=$?

[ $M -ne 0 -a $G -eq 0 ]
