#!/bin/bash
#
# Raid5: Inject data stripe corruption and fix them using scrub.
# 
# Script will perform the following:
# 1) Create Raid5 using 3 loopback devices.
# 2) Ensure file layout is created in a predictable manner. 
#    Each data stripe(64KB) should uniquely start with 'DNxxxx',   
#    where N represents the data stripe number.(ex:D0xxxx,D1xxxx etc)
# 3) Once file is created with specific layout, gather data stripe details 
#    like devicename, position and actual on-disk data.
# 4) Now use 'dd' to verify the data-stripe against its expected value.
# 5) At this stage, gather possible parity location for this specific data
#    stripe. The parity values is pre-computed for known data-stripes.
# 6) Inject corruption into given data-stripe by zero'ing out its first 4 bytes.
# 7) After injecting corruption, running online-scrub is expected to fix 
#    the corrupted data stripe with the help of parity block and 
#    corresponding data stripe. 
# 8) If scrub successful, verify the data stripe has original un-corrupted value.
# 9) If scrub successful, re-generate list of possible parity blocks, this
#    list must match one prepared in step 5. Otherwise report it as possible parity 
#    bug issue.
# 10) If no issues found, cleanup files and devices. Repeat the process for 
#    different file size and data-stripe.
#
#  Note: This script doesn't handle parity block corruption.

source $TOP/tests/common

check_prereq btrfs
check_prereq mkfs.btrfs

setup_root_helper
prepare_test_dev 512M

ndevs=3
declare -a devs
device_name=""
stripe_offset=""
stripe_content=""
known_parity_value="00010000000000000000000000000000"

LAYOUT_TMP=$(mktemp --tmpdir btrfs-progs-raid5-file.layoutXXXXXX)
STRIPEINFO_TMP=$(mktemp --tmpdir btrfs-progs-raid5-file.infoXXXXXX)
PARITYINFO1_TMP=$(mktemp --tmpdir btrfs-progs-raid5-initial.parityXXXXXX)
PARITYINFO2_TMP=$(mktemp --tmpdir btrfs-progs-raid5-end.parityXXXXXX)

prepare_devices()
{
	for i in `seq $ndevs`; do
		touch img$i
		chmod a+rw img$i
		truncate -s0 img$i
		truncate -s512M img$i
		devs[$i]=`run_check_stdout $SUDO_HELPER losetup --find --show img$i`
	done
}

cleanup_devices()
{
	for dev in ${devs[@]}; do
		run_check $SUDO_HELPER losetup -d $dev
	done
	for i in `seq $ndevs`; do
		truncate -s0 img$i
	done
	run_check $SUDO_HELPER losetup --all
}

test_do_mkfs()
{
	run_check $SUDO_HELPER $TOP/mkfs.btrfs -f	\
		$@
}

test_mkfs_multi()
{
	test_do_mkfs $@ ${devs[@]}
}

#$1 Filename
#$2 Expected no.of data stripes for the file.
create_layout(){
	fname=$1
	size=$(( $2 * 65536 ))
	n=0
	bs_value=1
	stripe=0
	while (( $n < $size ))
	do
		if [ $(( $n % 65536 )) -eq 0 ]; then
			val='D'$stripe
			echo -n $val
        		stripe=$(( $stripe+1 ))
			# ensure proper value		
			bs_value=`echo "${#val}"` 
        	else
			echo -n 'x'
			bs_value=1
		fi
        n=$(( $n+$bs_value ))
	done | dd of="$TEST_MNT"/$fname bs=$bs_value conv=notrunc &> /dev/null
}

find_data_stripe_details(){
	for dev in ${devs[@]}; do
		echo $dev >> $LAYOUT_TMP
		$SUDO_HELPER cat $dev | hexdump -e '"%010_ad|" 16/1 "%_p" "|\n"' |
		grep -P 'D[0-9]+xx'  >> $LAYOUT_TMP
	done
}
#$1 - file to save possible parity locations
find_parity_stripe_count(){
	PARITY_COUNT=$1
	for dev in ${devs[@]}; do
		echo "Possible parity stripes locations on device: $dev" >> $PARITY_COUNT
		$SUDO_HELPER cat $dev | hexdump -e '"%010_ad|" 16/1 "%02X" "\n"' |
		grep $known_parity_value >> $PARITY_COUNT
	done
}

#Collect data stripe information in a readable manner.
save_data_stripe_details(){
	devname=""
	for entry in `cat $LAYOUT_TMP`; do  
		echo $entry | grep -q '^\/dev\/loop' > /dev/null

		if [ $? -eq 0 ]; then
			devname=$entry	
		else
			echo $devname"|"$entry >> $STRIPEINFO_TMP
		fi
	done
	#sorting is not mandatory
	sort -t'|' -n -k3 $STRIPEINFO_TMP -o $STRIPEINFO_TMP
}
#Setup current known parity value
#$1 - integer to identify data-stripe range
set_parity_value(){
	num_digits=$1
	case $num_digits in
	1) #parity for D0xxxx to D9xxxx
	  known_parity_value="00010000000000000000000000000000"
	  ;;
	2) #parity for D10xxx to D99xxx
	  known_parity_value="00000100000000000000000000000000"
	  ;;
	esac
}


#Corrupt given data stripe
corrupt_data_stripe(){

	data_stripe_num=$1
	data_stripe_entry="D"${data_stripe_num}"xxxx"
	stripe_entry=`grep "${data_stripe_entry}" $STRIPEINFO_TMP`

	#Each entry will have format like "device|position|16-byte content"
	#Example: /dev/loop1|0063176704|D0xxxxxxxxxxxxxx|
	device_name=$(echo $stripe_entry | awk -F"|" '{print $1}')
	stripe_offset=$(echo $stripe_entry | awk -F"|" '{print $2}')
	#Remove leading zeros
	stripe_offset=$(echo $stripe_offset | sed 's/^0*//')
	stripe_content=$(echo $stripe_entry | awk -F"|" '{print $3}')

	echo "Corrupting $device_name at position $stripe_offset \
	which has $stripe_content" >> "$RESULTS"

	#verify the value at this position 
	original_value=$($SUDO_HELPER dd 2>/dev/null if=$device_name bs=1 \
	count=16 skip=$stripe_offset)

	if [ $original_value != $stripe_content ];then
		 echo "$original_value != $stripe_content"
		_fail "Data stripe mismatch. Possible use of incorrect block."
	else
		echo "Found on-disk value: $original_value " >> "$RESULTS"
	fi
	num_digits=$(echo $original_value | grep -o "[0-9]" | grep -c "")
	set_parity_value $num_digits
	find_parity_stripe_count $PARITYINFO1_TMP

	#Corrupt the given data stripe
	$SUDO_HELPER dd if=/dev/zero of=$device_name bs=1 count=4 \
	seek=$stripe_offset conv=notrunc &> /dev/null

	#Fetch value again.
	corrupted_value=$($SUDO_HELPER dd 2>/dev/null if=$device_name \
	bs=1 count=16 skip=$stripe_offset)

	if [ $corrupted_value == $original_value ];then
		 echo "original:$original_value corrupted:$corrupted_value"
		_fail "Corruption failed. Possible use of incorrect block."
	else
		echo "Corruption completed at $stripe_offset" >> "$RESULTS"
	fi
}

#Verify data stripe after scrub
verify_data_stripe(){

	value_after_scrub=$($SUDO_HELPER dd 2>/dev/null if=$device_name bs=1 \
	count=16 skip=$stripe_offset)
	if [ $value_after_scrub != $stripe_content ];then
		echo  "Found on-disk value: $value_after_scrub" >> "$RESULTS"
		_fail "Scrub failed to fix data stripe corruption."
	else
		echo "Scrub corrected value: $value_after_scrub" >> "$RESULTS"
	fi
}

#Verify parity stripe count after scrub
verify_parity_stripe(){
	find_parity_stripe_count $PARITYINFO2_TMP
	parity_stripe_count_before=$(md5sum $PARITYINFO1_TMP | cut -f1 -d' ')
	parity_stripe_count_after=$(md5sum $PARITYINFO2_TMP  | cut -f1 -d' ')
	if [ "$parity_stripe_count_before" != "$parity_stripe_count_after" ];then
		echo "Mismatch between $PARITYINFO1_TMP $PARITYINFO2_TMP, please check."
		diff $PARITYINFO1_TMP $PARITYINFO2_TMP
		_fail "Scrub corrupted parity stripe."
	else
		echo "Parity stripe check passed." >> "$RESULTS"
	fi
}


#$1 Filename
#$2 File with 'n' no.of data stripes
#$3 Data stripe to corrupt
test_raid5_datastripe_corruption(){
	filename=$1
	stripe_num=$2
	test_stripe=$3

	prepare_devices
	dev1=${devs[1]}
	dev2=${devs[2]}
	dev3=${devs[3]}

	test_mkfs_multi -d raid5 -m raid5
	run_check $SUDO_HELPER mount $dev1 $TEST_MNT
	create_layout $filename $stripe_num
	run_check $SUDO_HELPER umount "$TEST_MNT"

	#Gather data stripe informations like specific device,offset 
	find_data_stripe_details
	save_data_stripe_details
	corrupt_data_stripe $test_stripe

	#Mount the device and start scrub
	run_check $SUDO_HELPER mount $dev1 $TEST_MNT
	run_check $SUDO_HELPER btrfs scrub start $TEST_MNT
	#Introduce delay, hopefully scrubbing will be finished.
	sleep 10 

	#Validate 
	verify_data_stripe
	verify_parity_stripe

	#cleanup
	run_check $SUDO_HELPER umount "$TEST_MNT"
	cleanup_devices
	rm -f $LAYOUT_TMP
	rm -f $STRIPEINFO_TMP
}


test_raid5_datastripe_corruption file128k.txt 2 1 #file with 2 stripe,corrupt 1st.
test_raid5_datastripe_corruption file192k.txt 3 2 #file with 3 stripe,corrupt 2nd.
test_raid5_datastripe_corruption file256k.txt 4 3
test_raid5_datastripe_corruption file512k.txt 8 6
test_raid5_datastripe_corruption file768k.txt 12 10
test_raid5_datastripe_corruption file1m.txt 16 14 #1MB file, corrupt 14th stripe.
test_raid5_datastripe_corruption file2m.txt 32 23 #2MB file, corrupt 23rd stripe
