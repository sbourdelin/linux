#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+

get_secureboot_mode()
{
	EFIVARFS="/sys/firmware/efi/efivars"
	# Make sure that efivars is mounted in the normal location
	if ! grep -q "^\S\+ $EFIVARFS efivarfs" /proc/mounts; then
		echo "$TEST: efivars is not mounted on $EFIVARFS" >&2
		exit $ksft_skip
	fi

	# Get secureboot mode
	file="$EFIVARFS/SecureBoot-*"
	if [ ! -e $file ]; then
		echo "$TEST: unknown secureboot mode" >&2
		exit $ksft_skip
	fi
	return `hexdump $file | awk '{print substr($4,length($4),1)}'`
}
