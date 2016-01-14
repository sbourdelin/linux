#!/bin/bash

if [ "$1" != "oldconfig" ]; then
	exit 0
fi

srctree=$2
ARCH="$3"
UNAME_RELEASE=$(uname -r)

CONFIGS=".config /lib/modules/$UNAME_RELEASE/.config /etc/kernel-config /boot/config-$UNAME_RELEASE"

if [ "$ARCH" = "X86_32" ]; then
	CONFIGS="$CONFIGS $srctree/arch/x86/configs/i386_defconfig"
else
	CONFIGS="$CONFIGS $srctree/arch/x86/configs/x86_64_defconfig"
fi

for c in $CONFIGS;
do
	if [ -e $c ]; then
		OLD_CONFIG=$c
		break
	fi
done

if [ -z "$OLD_CONFIG" ]; then exit 0; fi

# Check optimal microcode loader .config settings
if ! grep -v "^#" $OLD_CONFIG | grep -q MICROCODE; then
	exit 0
fi

MSG="\nYou have CONFIG_MICROCODE enabled without BLK_DEV_INITRD. The preferred\n\
way is to enable it and make sure microcode is added to your initrd as\n\
explained in Documentation/x86/early-microcode.txt. This is also the\n\
most tested method as the majority of distros do it. Alternatively, and\n\
if you don't want to enable modules, you should make sure the microcode\n\
is built into the kernel.\n"

if ! grep -v "^#" $OLD_CONFIG | grep -q BLK_DEV_INITRD; then
	echo -e $MSG
	read -p "Press any key... "
fi
