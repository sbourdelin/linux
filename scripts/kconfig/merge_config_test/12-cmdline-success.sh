#!/bin/sh

. "$(dirname $0)/common.sh"

# Turn off an option

merge CONFIG_64BIT=n
M=$?

check CONFIG_64BIT=y
G=$?

[ $M -eq 0 -a $G -ne 0 ]
