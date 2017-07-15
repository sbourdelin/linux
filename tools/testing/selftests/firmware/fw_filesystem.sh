#!/bin/sh
# This validates that the kernel will load firmware out of its list of
# firmware locations on disk. Since the user helper does similar work,
# we reset the custom load directory to a location the user helper doesn't
# know so we can be sure we're not accidentally testing the user helper.
set -e

DIR=/sys/devices/virtual/misc/test_firmware
TEST_DIR=$(dirname $0)

test_modprobe()
{
	if [ ! -d $DIR ]; then
		echo "$0: $DIR not present"
		echo "You must have the following enabled in your kernel:"
		cat $TEST_DIR/config
		exit 1
	fi
}

trap "test_modprobe" EXIT

if [ ! -d $DIR ]; then
	modprobe test_firmware
fi

# CONFIG_FW_LOADER_USER_HELPER has a sysfs class under /sys/class/firmware/
# These days most distros enable CONFIG_FW_LOADER_USER_HELPER but disable
# CONFIG_FW_LOADER_USER_HELPER_FALLBACK. We use /sys/class/firmware/ as an
# indicator for CONFIG_FW_LOADER_USER_HELPER.
HAS_FW_LOADER_USER_HELPER=$(if [ -d /sys/class/firmware/ ]; then echo yes; else echo no; fi)

if [ "$HAS_FW_LOADER_USER_HELPER" = "yes" ]; then
	OLD_TIMEOUT=$(cat /sys/class/firmware/timeout)
fi

OLD_FWPATH=$(cat /sys/module/firmware_class/parameters/path)

FWPATH=$(mktemp -d)
FW="$FWPATH/test-firmware.bin"

test_finish()
{
	if [ "$HAS_FW_LOADER_USER_HELPER" = "yes" ]; then
		echo "$OLD_TIMEOUT" >/sys/class/firmware/timeout
	fi
	echo -n "$OLD_PATH" >/sys/module/firmware_class/parameters/path
	rm -f "$FW"
	rmdir "$FWPATH"
}

trap "test_finish" EXIT

if [ "$HAS_FW_LOADER_USER_HELPER" = "yes" ]; then
	# Turn down the timeout so failures don't take so long.
	echo 1 >/sys/class/firmware/timeout
fi

# Set the kernel search path.
echo -n "$FWPATH" >/sys/module/firmware_class/parameters/path

# This is an unlikely real-world firmware content. :)
echo "ABCD0123" >"$FW"

NAME=$(basename "$FW")

if printf '\000' >"$DIR"/trigger_request 2> /dev/null; then
	echo "$0: empty filename should not succeed" >&2
	exit 1
fi

if printf '\000' >"$DIR"/trigger_async_request 2> /dev/null; then
	echo "$0: empty filename should not succeed (async)" >&2
	exit 1
fi

# Request a firmware that doesn't exist, it should fail.
if echo -n "nope-$NAME" >"$DIR"/trigger_request 2> /dev/null; then
	echo "$0: firmware shouldn't have loaded" >&2
	exit 1
fi
if diff -q "$FW" /dev/test_firmware >/dev/null ; then
	echo "$0: firmware was not expected to match" >&2
	exit 1
else
	if [ "$HAS_FW_LOADER_USER_HELPER" = "yes" ]; then
		echo "$0: timeout works"
	fi
fi

# This should succeed via kernel load or will fail after 1 second after
# being handed over to the user helper, which won't find the fw either.
if ! echo -n "$NAME" >"$DIR"/trigger_request ; then
	echo "$0: could not trigger request" >&2
	exit 1
fi

# Verify the contents are what we expect.
if ! diff -q "$FW" /dev/test_firmware >/dev/null ; then
	echo "$0: firmware was not loaded" >&2
	exit 1
else
	echo "$0: filesystem loading works"
fi

# Try the asynchronous version too
if ! echo -n "$NAME" >"$DIR"/trigger_async_request ; then
	echo "$0: could not trigger async request" >&2
	exit 1
fi

# Verify the contents are what we expect.
if ! diff -q "$FW" /dev/test_firmware >/dev/null ; then
	echo "$0: firmware was not loaded (async)" >&2
	exit 1
else
	echo "$0: async filesystem loading works"
fi

### Batched requests tests
test_config_present()
{
	if [ ! -f $DIR/reset ]; then
		echo "Configuration triggers not present, ignoring test"
		exit 0
	fi
}

# Defaults:
# send_uevent is 1
# sync_direct is 0
config_reset()
{
	echo 1 >  $DIR/reset
}

config_set_sync_direct()
{
	echo 1 >  $DIR/config_sync_direct
}

config_unset_sync_direct()
{
	echo 0 >  $DIR/config_sync_direct
}

config_set_uevent()
{
	echo 1 >  $DIR/config_send_uevent
}

config_unset_uevent()
{
	echo 0 >  $DIR/config_send_uevent
}

config_trigger_sync()
{
	echo -n 1 > $DIR/trigger_batched_requests 2>/dev/null
}

config_trigger_async()
{
	echo -n 1 > $DIR/trigger_batched_requests_async 2> /dev/null
}

test_batched_request_firmware()
{
	echo -n "Batched request_firmware() try #$1: "
	config_reset
	config_trigger_sync
	echo "OK"
}

test_batched_request_firmware_direct()
{
	echo -n "Batched request_firmware_direct() try #$1: "
	config_reset
	config_set_sync_direct
	config_trigger_sync
	echo "OK"
}

test_request_firmware_nowait_uevent()
{
	echo -n "Batched request_firmware_nowait(uevent=true) try #$1: "
	config_reset
	config_trigger_async
	echo "OK"
}

test_request_firmware_nowait_custom()
{
	echo -n "Batched request_firmware_nowait(uevent=false) try #$1: "
	config_unset_uevent
	config_trigger_async
	echo "OK"
}

# Only continue if batched request triggers are present on the
# test-firmware driver
test_config_present

for i in $(seq 1 5); do
	test_batched_request_firmware $i
done

for i in $(seq 1 5); do
	test_batched_request_firmware_direct $i
done

for i in $(seq 1 5); do
	test_request_firmware_nowait_uevent $i
done

for i in $(seq 1 5); do
	test_request_firmware_nowait_custom $i
done

exit 0
