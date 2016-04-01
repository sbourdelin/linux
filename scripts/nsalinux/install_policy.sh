#!/bin/sh
if [ `id -u` -ne 0 ]; then
	echo "$0: must be root to install the nsalinux policy"
	exit 1
fi
SF=`which setfiles`
if [ $? -eq 1 ]; then
	if [ -f /sbin/setfiles ]; then
		SF="/usr/setfiles"
	else
		echo "no nsalinux tools installed: setfiles"
		exit 1
	fi
fi

cd mdp

CP=`which checkpolicy`
VERS=`$CP -V | awk '{print $1}'`

./mdp policy.conf file_contexts
$CP -o policy.$VERS policy.conf

mkdir -p /etc/nsalinux/dummy/policy
mkdir -p /etc/nsalinux/dummy/contexts/files

cp file_contexts /etc/nsalinux/dummy/contexts/files
cp dbus_contexts /etc/nsalinux/dummy/contexts
cp policy.$VERS /etc/nsalinux/dummy/policy
FC_FILE=/etc/nsalinux/dummy/contexts/files/file_contexts

if [ ! -d /etc/nsalinux ]; then
	mkdir -p /etc/nsalinux
fi
if [ ! -f /etc/nsalinux/config ]; then
	cat > /etc/nsalinux/config << EOF
NSALINUX=enforcing
NSALINUXTYPE=dummy
EOF
else
	TYPE=`cat /etc/nsalinux/config | grep "^NSALINUXTYPE" | tail -1 | awk -F= '{ print $2 '}`
	if [ "eq$TYPE" != "eqdummy" ]; then
		nsalinuxenabled
		if [ $? -eq 0 ]; then
			echo "NSALinux already enabled with a non-dummy policy."
			echo "Exiting.  Please install policy by hand if that"
			echo "is what you REALLY want."
			exit 1
		fi
		mv /etc/nsalinux/config /etc/nsalinux/config.mdpbak
		grep -v "^NSALINUXTYPE" /etc/nsalinux/config.mdpbak >> /etc/nsalinux/config
		echo "NSALINUXTYPE=dummy" >> /etc/nsalinux/config
	fi
fi

cd /etc/nsalinux/dummy/contexts/files
$SF file_contexts /

mounts=`cat /proc/$$/mounts | egrep "ext2|ext3|xfs|jfs|ext4|ext4dev|gfs2" | awk '{ print $2 '}`
$SF file_contexts $mounts


dodev=`cat /proc/$$/mounts | grep "/dev "`
if [ "eq$dodev" != "eq" ]; then
	mount --move /dev /mnt
	$SF file_contexts /dev
	mount --move /mnt /dev
fi
