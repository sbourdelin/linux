#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# Test bpf_rawir_event over rc-loopback. Steps for the test:
#
# 1. Find the /dev/lircN device for rc-loopback
# 2. Attach bpf_rawir_event program which decodes some IR.
# 3. Send some IR to the same IR device; since it is loopback, this will
#    end up in the bpf program
# 4. bpf program should decode IR and report keycode
# 5. We can read keycode from same /dev/lirc device

GREEN='\033[0;92m'
RED='\033[0;31m'
NC='\033[0m' # No Color

modprobe rc-loopback

for i in /sys/class/rc/rc*
do
	if grep -q DRV_NAME=rc-loopback $i/uevent
	then
		LIRCDEV=$(grep DEVNAME= $i/lirc*/uevent | sed sQDEVNAME=Q/dev/Q)
	fi
done

if [ -n $LIRCDEV ];
then
	TYPE=rawir_event
	./test_rawir_event_user $LIRCDEV
	ret=$?
        if [ $ret -ne 0 ]; then
                echo -e ${RED}"FAIL: $TYPE"${NC}
                return 1
        fi
        echo -e ${GREEN}"PASS: $TYPE"${NC}
fi
