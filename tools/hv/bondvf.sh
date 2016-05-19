#!/bin/bash

# This example script creates bonding network devices based on synthetic NIC
# (the virtual network adapter usually provided by Hyper-V) and the matching
# VF NIC (SRIOV virtual function). So the synthetic NIC and VF NIC can
# function as one network device, and fail over to the synthetic NIC if VF is
# down.
#
# Usage:
# - After configured vSwitch and vNIC with SRIOV, start Linux virtual
#   machine (VM)
# - Run this scripts on the VM. It will create configuration files in
#   /etc/sysconfig/network-scripts/
# - Reboot the VM, so that the bonding config are enabled.
#
# The config files are DHCP by default. You may edit them if you need to change
# to Static IP or change other settings. The file name is, for example:
#   /etc/sysconfig/network-scripts/ifcfg-bond0
#
# Each Distro is expected to implement this script in a distro specific
# fashion.
#
# This example script is based on a RHEL environment.
#
# Here is an example of the bonding configuration file:
#   DEVICE=bond0
#   TYPE=Bond
#   BOOTPROTO=dhcp
#   ONBOOT=yes
#   NM_CONTROLLED=no
#   PEERDNS=yes
#   IPV6INIT=yes
#   BONDING_MASTER=yes
#   BONDING_OPTS="mode=active-backup miimon=100 primary=eth1"
#

sysdir=/sys/class/net
cfgdir=/etc/sysconfig/network-scripts
netvsc_cls={f8615163-df3e-46c5-913f-f2d2f965ed0e}
bondcnt=0

# Get a list of ethernet names
list_eth=(`cd $sysdir && ls -d */ | cut -d/ -f1 | grep -v bond`)
eth_cnt=${#list_eth[@]}

echo List of net devices:

# Get the MAC addresses
for (( i=0; i < $eth_cnt; i++ ))
do
	list_mac[$i]=`cat $sysdir/${list_eth[$i]}/address`
	echo ${list_eth[$i]}, ${list_mac[$i]}
done

# Find NIC with matching MAC
for (( i=0; i < $eth_cnt-1; i++ ))
do
	for (( j=i+1; j < $eth_cnt; j++ ))
	do
		if [ "${list_mac[$i]}" = "${list_mac[$j]}" ]
		then
			list_match[$i]=${list_eth[$j]}
			break
		fi
	done
done

function create_eth_cfg {
	local fn=$cfgdir/ifcfg-$1

	echo creating: $fn for $2

	rm -f $fn
	echo DEVICE=$1 >>$fn
	echo TYPE=Ethernet >>$fn
	echo BOOTPROTO=none >>$fn
	echo ONBOOT=yes >>$fn
	echo NM_CONTROLLED=no >>$fn
	echo PEERDNS=yes >>$fn
	echo IPV6INIT=yes >>$fn
	echo MASTER=$2 >>$fn
	echo SLAVE=yes >>$fn
}

function create_bond_cfg {
	local fn=$cfgdir/ifcfg-$1

	echo $'\nBond name:' $1
	echo creating: $fn with primary slave: $2

	rm -f $fn
	echo DEVICE=$1 >>$fn
	echo TYPE=Bond >>$fn
	echo BOOTPROTO=dhcp >>$fn
	echo ONBOOT=yes >>$fn
	echo NM_CONTROLLED=no >>$fn
	echo PEERDNS=yes >>$fn
	echo IPV6INIT=yes >>$fn
	echo BONDING_MASTER=yes >>$fn
	echo BONDING_OPTS=\"mode=active-backup miimon=100 primary=$2\" >>$fn
}

function create_bond {
	local bondname=bond$bondcnt

	local class_id1=`cat $sysdir/$1/device/class_id 2>/dev/null`
	local class_id2=`cat $sysdir/$2/device/class_id 2>/dev/null`

	if [ "$class_id1" = "$netvsc_cls" ]
	then
		create_bond_cfg $bondname $2
	elif [ "$class_id2" = "$netvsc_cls" ]
	then
		create_bond_cfg $bondname $1
	else
		return 0
	fi

	create_eth_cfg $1 $bondname
	create_eth_cfg $2 $bondname

	let bondcnt=bondcnt+1
}

for (( i=0; i < $eth_cnt-1; i++ ))
do
        if [ -n "${list_match[$i]}" ]
        then
		create_bond ${list_eth[$i]} ${list_match[$i]}
        fi
done

