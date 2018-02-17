#!/bin/bash
#
# helper script which reads all kconfig keys from the kernel .config file into
# a bash associative array.
# By testing ${KERNEL_CONFIG[CONFIG_FOO_BAR]} shell scripts can check whether
# CONFIG_FOO_BAR is set in .config or not.
#

declare -A KERNEL_CONFIG

for cfg_ent in $(awk -F= '/^CONFIG_[A-Z0-9_]+=/{print $1}' < ${KCONFIG_CONFIG})
do
	KERNEL_CONFIG[${cfg_ent}]="$cfg_ent"
done
