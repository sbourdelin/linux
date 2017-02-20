#!/bin/bash
#
# Raid5: Inject data stripe corruption and fix them using scrub.
# 
# Script will perform the following:
# 1) Create Raid5 using 3 loopback devices.
# 2) Ensure file layout is created in a predictable manner. 
#    Each data stripe(64KB) should uniquely start with 'DNxxxx',   
#    where N represents the data stripe number.(ex:D0xxxx,D1xxxx etc)
# 3) Once file is created with specific layout, check whether file 
#    has single extent. At the moment, script wont handle multi-extent
#    files.
# 4) If file has single extent with the help of 'dump-tree' compute data and 
#    parity stripe details like devicename, position and actual on-disk data.
# 5) $STRIPEINFO_COMPLETE file will have all necessary data at this stage.
# 6) Inject corruption into given data-stripe by zero'ing out its first 4 bytes.
# 7) After injecting corruption, running online-scrub is expected to fix 
#    the corrupted data stripe with the help of parity block and 
#    corresponding data stripe. 
# 8) If scrub successful, verify the data stripe has original un-corrupted value.
# 9) If scrub successful, verify parity stripe is valid, otherwise its a parity bug.
# 10) If no issues found, cleanup files and devices. Repeat the process for 
#    different file size and data-stripe.
#
#  Note: This script corrupts only data-stripe blocks. 
#  Known Limitations (will be addressed later):
#         - Script expects even number of data-stripes in file.
#         - Script expects the file to have single extent.

source $TOP/tests/common

check_prereq btrfs
check_prereq mkfs.btrfs
check_prereq btrfs-debugfs

setup_root_helper
prepare_test_dev 1024M

ndevs=3
declare -a devs
declare -a parity_offsets
stripe_entry=""
device_name=""
stripe_offset=""
stripe_content=""

#Complete stripe layout
STRIPEINFO_COMPLETE=$(mktemp --tmpdir btrfs-progs-raid5-stripe-complete.infoXXXXXX)
#dump-tree output file
DUMPTREE_OUTPUT=$(mktemp --tmpdir btrfs-progs-raid5-tree-dump.infoXXXXXX)
#tmp files
STRIPEINFO_PARTIAL=$(mktemp --tmpdir btrfs-progs-raid5-stripe-partial.infoXXXXXX)
STRIPE_TMP=$(mktemp --tmpdir btrfs-progs-raid5-stripetmp.infoXXXXXX)
MULTI_EXTENT_CHECK=$(mktemp --tmpdir btrfs-progs-raid5-extent-check.infoXXXXXX)
EXTENT_WITH_SIZE=$(mktemp --tmpdir btrfs-progs-raid5-extent-size.infoXXXXXX)
PARITY_LOC1=$(mktemp --tmpdir btrfs-progs-raid5-parity-loc1.infoXXXXXX)
PARITY_LOCATION=$(mktemp --tmpdir btrfs-progs-raid5-parity-locations.infoXXXXXX)


prepare_devices()
{
	for i in `seq $ndevs`; do
		touch img$i
		chmod a+rw img$i
		truncate -s0 img$i
		truncate -s512M img$i
		devs[$i]=`run_check_stdout $SUDO_HELPER losetup --find --show img$i`
	done
	truncate -s0 $STRIPE_TMP
	truncate -s0 $STRIPEINFO_PARTIAL
	truncate -s0 $STRIPEINFO_COMPLETE
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
	done | dd of=/tmp/$fname bs=$bs_value conv=notrunc &> /dev/null
	cp /tmp/$fname $TEST_MNT
	rm -f /tmp/$fname
}

# $1,$2,$3 - available device offsets
find_parity_device(){
	parity_offsets[1]=$1
	parity_offsets[2]=$2
	parity_offsets[3]=$3
	pdev=""

	for dev in ${devs[@]}; do
		grep $dev $STRIPEINFO_PARTIAL &>/dev/null
		if [ $? -ne 0 ];then
			pdev=$dev
		fi
	done
	#find parity location
	truncate -s0 $PARITY_LOC1 
	truncate -s0 $PARITY_LOCATION
	awk -F'|' '{print $2}' $STRIPEINFO_PARTIAL > $PARITY_LOC1

	for addr in ${parity_offsets[@]}; do
		echo $addr >> $PARITY_LOCATION
	done
	sort $PARITY_LOCATION -o $PARITY_LOCATION 
	sort $PARITY_LOC1 -o $PARITY_LOC1

	poffset=$(comm -23 $PARITY_LOCATION $PARITY_LOC1)
	pdata=$($SUDO_HELPER dd 2>/dev/null if=$pdev bs=1 \
		count=16 skip=$poffset | hexdump -e '16/1 "%02x" "\n"')
	echo "$pdev|$poffset|$pdata" >> $STRIPEINFO_PARTIAL
}

# $1 - physical address
# $2 - pattern/digit to search
get_stripe_contents(){
 	pysical_addr=$1
	search_digit=$2
	
	for dev in ${devs[@]}; do
		stripe_data=$($SUDO_HELPER dd 2>/dev/null if=$dev bs=1 \
		count=16 skip=$pysical_addr)
		echo $stripe_data | grep -E "D${search_digit}xxx" &>/dev/null
		if [ $? -eq 0 ]; then
			touch $STRIPE_TMP
			echo "$dev|$pysical_addr|$stripe_data" >> $STRIPE_TMP
			return 42
		fi
	done
}

#$1 - logical addr
#$2 - total stripes
logical_to_pysical(){
	logical_addr=$1
	total_stripe=$2

	chunk_startaddr=$(cat $DUMPTREE_OUTPUT | grep -A3 "FIRST_CHUNK_TREE CHUNK_ITEM" | \
	grep -B1 "\ DATA|RAID" | grep "FIRST_CHUNK_TREE" | \
	sed -n -r 's/.*CHUNK_ITEM (.*)\) itemoff .*/\1/p')
	chunk_length=$(cat $DUMPTREE_OUTPUT | grep -A3 "FIRST_CHUNK_TREE CHUNK_ITEM"  | \
	grep  "\ DATA|RAID" | sed -n -r 's/.*length (.*) owner .*/\1/p')


	stripe1_start=$(cat $DUMPTREE_OUTPUT | \
	grep -A10 "FIRST_CHUNK_TREE CHUNK_ITEM $chunk_startaddr" | grep "offset" | \
	awk '{print $6}' | tail -n3 | head -n1)
	stripe2_start=$(cat $DUMPTREE_OUTPUT | \
	grep -A10 "FIRST_CHUNK_TREE CHUNK_ITEM $chunk_startaddr" | grep "offset" | \
	awk '{print $6}' | tail -n2 | head -n1)
	stripe3_start=$(cat $DUMPTREE_OUTPUT | \
	grep -A10 "FIRST_CHUNK_TREE CHUNK_ITEM $chunk_startaddr" | grep "offset" | \
	awk '{print $6}' | tail -n1 | head -n1)

	#search_single_extent $logical_addr
	chunk_offset=$(( $logical_addr - $chunk_startaddr ))
	chunk_slot=$(( $chunk_offset / 131072 ))

	ds1_pysical_addr=$(( $stripe1_start + $chunk_slot * 65536 )) 
	ds2_pysical_addr=$(( $stripe2_start + $chunk_slot * 65536 ))
	ds3_pysical_addr=$(( $stripe3_start + $chunk_slot * 65536 ))

	d1=0
	while (( $total_stripe > 1))
	do
			for addr in $ds1_pysical_addr $ds2_pysical_addr $ds3_pysical_addr; do
				get_stripe_contents $addr $d1
				fun_ret=$?
				if [ $fun_ret == 42 ];then
					break
				fi
			done
			# Search 2nd data-stripe
			d1=$(( $d1 + 1 ))
			for addr in $ds1_pysical_addr $ds2_pysical_addr $ds3_pysical_addr; do
				get_stripe_contents $addr $d1
				fun_ret=$?
				if [ $fun_ret == 42 ];then
					break
				fi
			done
			d1=$(( $d1 + 1 ))
				#Find parity device 
				uniq  $STRIPE_TMP > $STRIPEINFO_PARTIAL
				find_parity_device $ds1_pysical_addr $ds2_pysical_addr $ds3_pysical_addr
				cat $STRIPEINFO_PARTIAL >> $STRIPEINFO_COMPLETE
				truncate -s0 $STRIPE_TMP
				truncate -s0 $STRIPEINFO_PARTIAL
		total_stripe=$(( $total_stripe - 2 ))
		ds1_pysical_addr=$(( $ds1_pysical_addr +  65536 )) 
		ds2_pysical_addr=$(( $ds2_pysical_addr +  65536 )) 
		ds3_pysical_addr=$(( $ds3_pysical_addr +  65536 )) 
	done
}

#$1 - Total no.of chunks
#$2 - loop dev
#$3 - filename
gather_stripe_info(){
	total_stripe=$1
	loop_dev=$2
	filename=$3

	sleep 2
	run_check_stdout $SUDO_HELPER $TOP/btrfs inspect-internal dump-tree $loop_dev > "$DUMPTREE_OUTPUT"
	$TOP/btrfs-debugfs -f $TEST_MNT/$filename &> "$MULTI_EXTENT_CHECK"
	cat $MULTI_EXTENT_CHECK | awk '{print $6 "-" $4}' | grep -v '[a-z]' > "$EXTENT_WITH_SIZE"
	extents_count=$(cat $EXTENT_WITH_SIZE | wc -l)
	
	if [ $extents_count -ne 1 ];then
		echo "File with multiple extents found."
		for item in `cat $EXTENT_WITH_SIZE`;do
			logical_addr=$(echo $item | awk -F'-' {'print $1'})
			len=$(echo $item | awk -F'-' {'print $2'})
			total_stripe=$(( $len / 65536  ))
			echo "logical_addr is $logical_addr and leng=$len and total_stripe=$total_stripe";
		done
			_fail "Script doesnt handle multiple extents.Re-try again."
	else 
		echo "Single extent found." >> "$RESULTS"
		logical_addr=$(awk -F'-' '{print $1}' $EXTENT_WITH_SIZE)
		len=$(awk -F'-' '{print $2}' $EXTENT_WITH_SIZE)
		total_stripe=$(( $len / 65536  ))
		echo "logical_addr: $logical_addr length=$len \
		total_stripe=$total_stripe" >> "$RESULTS"
		logical_to_pysical $logical_addr $total_stripe
	fi	
}

#Corrupt given data stripe
corrupt_data_stripe(){

	data_stripe_num=$1
	data_stripe_entry="D"${data_stripe_num}"xxxx"
	stripe_entry=$(grep "${data_stripe_entry}" $STRIPEINFO_COMPLETE)

	#Each entry will have format like "device|position|16-byte content"
	#Example: /dev/loop1|0063176704|D0xxxxxxxxxxxxxx|
	device_name=$(echo $stripe_entry | awk -F"|" '{print $1}')
	stripe_offset=$(echo $stripe_entry | awk -F"|" '{print $2}')
	#Remove leading zeros
	stripe_offset=$(echo $stripe_offset | sed 's/^0*//')
	stripe_content=$(echo $stripe_entry | awk -F"|" '{print $3}')

	echo "$data_stripe_entry: Corrupting $device_name at position $stripe_offset \
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
	parity_stripe_entry=$(grep -A3 "$stripe_entry" $STRIPEINFO_COMPLETE | grep "10000000000")
        parity_device_name=$(echo $parity_stripe_entry | awk -F"|" '{print $1}')
        parity_stripe_offset=$(echo $parity_stripe_entry | awk -F"|" '{print $2}')

	echo "Parity_stripe_entry:$parity_stripe_entry" >> "$RESULTS"

        #Remove leading zeros
        parity_stripe_offset=$(echo $parity_stripe_offset | sed 's/^0*//')
        parity_stripe_content=$(echo $parity_stripe_entry | awk -F"|" '{print $3}')

	#get current parity value 
        parity_stripe_after_scrub=$($SUDO_HELPER dd 2>/dev/null if=$parity_device_name bs=1 \
                count=16 skip=$parity_stripe_offset | hexdump -e '16/1 "%02x" "\n"')

	echo "Parity: before=$parity_stripe_content after=$parity_stripe_after_scrub" >> "$RESULTS"
	if [ "$parity_stripe_content" != "$parity_stripe_after_scrub" ];then
		echo "Parity Mismatch: $parity_stripe_content $parity_stripe_after_scrub." >> "$RESULTS"
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
	echo "------------------RAID5 Corruption------------------" >> "$RESULTS"
	echo "Filename=$filename Total Stripes=$stripe_num \
	      Data Stripe to be corrupted=$test_stripe" >> "$RESULTS"
	   	
	prepare_devices
	dev1=${devs[1]}
	test_mkfs_multi -d raid5 -m raid5
	run_check $SUDO_HELPER mount $dev1 $TEST_MNT
	create_layout $filename $stripe_num
	cd $TEST_MNT && sync && sync && echo "3" > /proc/sys/vm/drop_caches && cd - &> /dev/null

	#Gather stripe info like specific device,offset,content
	gather_stripe_info $stripe_num $dev1 $filename
	run_check $SUDO_HELPER umount "$TEST_MNT"

	#Inject corruption
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
	rm -rf $STRIPEINFO_COMPLETE $DUMPTREE_OUTPUT $STRIPEINFO_PARTIAL $STRIPE_TMP
	rm -rf $MULTI_EXTENT_CHECK $EXTENT_WITH_SIZE $PARITY_LOC1 $PARITY_LOCATION
}


test_raid5_datastripe_corruption file128k.txt 2 1 #file with 2 stripe,corrupt 1st.
test_raid5_datastripe_corruption file256k.txt 4 3
test_raid5_datastripe_corruption file512k.txt 8 6
test_raid5_datastripe_corruption file768k.txt 12 10
test_raid5_datastripe_corruption file1m.txt 16 14 #1MB file, corrupt 14th stripe.
test_raid5_datastripe_corruption file2m.txt 32 23 #2MB file, corrupt 23rd stripe
test_raid5_datastripe_corruption file4m.txt 64 34 #4MB file
test_raid5_datastripe_corruption file8m.txt 128 100 #8MB file
