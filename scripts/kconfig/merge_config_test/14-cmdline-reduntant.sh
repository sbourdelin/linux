#!/bin/sh

. "$(dirname $0)/common.sh"

# Make sure redundant options are warned about

merge_r CONFIG_64BIT=n CONFIG_64BIT=n
M=$?

check CONFIG_64BIT=y
G=$?

[ $M -ne 0 -a $G -ne 0 ]
