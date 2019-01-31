#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
#
# Loading a kernel image via the kexec_file_load syscall can verify either
# the IMA signature stored in the security.ima xattr or the PE signature,
# both signatures depending on the IMA policy, or none.
#
# To determine whether the kernel image is signed, this test depends
# on pesign and getfattr.  This test also requires the kernel to be
# built with CONFIG_IKCONFIG enabled and either CONFIG_IKCONFIG_PROC
# enabled or access to the extract-ikconfig script.

VERBOSE=1
EXTRACT_IKCONFIG=$(ls /lib/modules/`uname -r`/source/scripts/extract-ikconfig)
IKCONFIG=/tmp/config-`uname -r`
PROC_CONFIG="/proc/config.gz"
KERNEL_IMAGE="/boot/vmlinuz-`uname -r`"
PESIGN=/usr/bin/pesign
GETFATTR=/usr/bin/getfattr

TEST="$0"
. ./common_lib.sh

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4

kconfig_enabled()
{
	RC=0
	egrep -q $1 $IKCONFIG
	if [ $? -eq 0 ]; then
		RC=1
	fi
	return $RC
}

# policy rule format: action func=<keyword> [appraise_type=<type>]
check_ima_policy()
{
	IMA_POLICY=/sys/kernel/security/ima/policy

	RC=0
	if [ $# -eq 3 ]; then
		grep -e $2 $IMA_POLICY | grep -e "^$1.*$3" 2>&1 >/dev/null
	else
		grep -e $2 $IMA_POLICY | grep -e "^$1" 2>&1 >/dev/null
	fi
	if [ $? -eq 0 ]; then
		RC=1
	fi
	return $RC
}

check_kconfig_options()
{
	# Attempt to get the kernel config first via proc, and then by
	# extracting it from the kernel image using scripts/extract-ikconfig.
	if [ ! -f $PROC_CONFIG ]; then
		modprobe configs 2>/dev/null
	fi
	if [ -f $PROC_CONFIG ]; then
		cat $PROC_CONFIG > $IKCONFIG
	fi

	if [ ! -f $IKCONFIG ]; then
		if [ ! -f $EXTRACT_IKCONFIG ]; then
			echo "$TEST: requires access to extract-ikconfig" >&2
			exit $ksft_skip
		fi

		$EXTRACT_IKCONFIG $KERNEL_IMAGE > $IKCONFIG
		kconfig_enabled "CONFIG_IKCONFIG=y"
		if [ $? -eq 0 ]; then
			echo "$TEST: requires the kernel to be built with CONFIG_IKCONFIG" >&2
			exit $ksft_skip
		fi
	fi

	kconfig_enabled "CONFIG_KEXEC_BZIMAGE_VERIFY_SIG=y"
	pe_sig_required=$?
	if [ $VERBOSE -ne 0 ] && [ $pe_sig_required -eq 1 ]; then
		echo "$TEST: [INFO] PE signed kernel image required"
	fi

	kconfig_enabled "CONFIG_IMA_APPRAISE_REQUIRE_KEXEC_SIGS=y"
	ima_sig_required=$?
	if [ $VERBOSE -ne 0 ] && [ $ima_sig_required -eq 1 ]; then
		echo "$TEST: [INFO] IMA kernel image signature required"
	fi

	kconfig_enabled "CONFIG_IMA_ARCH_POLICY=y"
	arch_policy=$?
	if [ $VERBOSE -ne 0 ] && [ $arch_policy -eq 1 ]; then
		echo "$TEST: [INFO] architecture specific policy enabled"
	fi

	kconfig_enabled "CONFIG_INTEGRITY_PLATFORM_KEYRING=y"
	platform_keyring=$?
	if [ $VERBOSE -ne 0 ] && [ $platform_keyring -eq 1 ]; then
		echo "$TEST: [INFO] platform kerying enabled"
	fi

	kconfig_enabled "CONFIG_IMA_READ_POLICY=y"
	ima_read_policy=$?
	if [ $VERBOSE -ne 0 ] && [ $ima_read_policy -eq 1 ]; then
		echo "$TEST: [INFO] userspace can read IMA policy"
	fi
	rm $IKCONFIG
}

check_for_apps()
{
	if [ ! -f $PESIGN ]; then
		PESIGN=$(which pesign 2>/dev/null)
		if [ $?	-eq 1 ]; then
			echo "$TEST: requires pesign" >&2
			exit $ksft_skip
		else
			echo "$TEST: [INFO] found $PESIGN"
		fi
	fi

	if [ ! -f $GETFATTR ]; then
		GETFATTR=$(which getfattr 2>/dev/null)
		if [ $?	-eq 1 ]; then
			echo "$TEST: requires getfattr" >&2
			exit $ksft_skip
		else
			echo "$TEST: [INFO] found $GETFATTR"
		fi
	fi
}

check_runtime()
{
	get_secureboot_mode
	secureboot=$?
	if [ $VERBOSE -ne 0 ] && [ $secureboot -eq 1 ]; then
		echo "$TEST: [INFO] secure boot mode enabled"
	fi
	# The builtin "secure_boot" or custom policies might require an
	# IMA signature.  Check the runtime appraise policy rules
	# (eg. <securityfs>/ima/policy).  Policy rules are walked
	# sequentially.  As a result, a policy rule may be defined,
	# but might not necessarily be used.  This test assumes if a
	# policy rule is specified, that is the intent.
	if [ $ima_sig_required -eq 0 ] && [ $ima_read_policy -eq 1 ]; then
		check_ima_policy "appraise" "func=KEXEC_KERNEL_CHECK" \
			"appraise_type=imasig"
		ima_sig_required=$?
		if [ $VERBOSE -ne 0 ] && [ $ima_sig_required -eq 1 ]; then
			echo "$TEST: [INFO] IMA signature required"
		fi
	fi
}

check_for_sigs()
{
	pe_signed=0
	$PESIGN -i $KERNEL_IMAGE --show-signature | grep -q "No signatures"
	pe_signed=$?
	if [ $VERBOSE -ne 0 ]; then
		if [ $pe_signed -eq 1 ]; then
			echo "$TEST: [INFO] kexec kernel image PE signed"
		else
			echo "$TEST: [INFO] kexec kernel image not PE signed"
		fi
	fi

	ima_signed=0
	line=$($GETFATTR -n security.ima -e hex --absolute-names $KERNEL_IMAGE 2>&1)
	echo $line | grep -q "security.ima=0x03"
	if [ $? -eq 0 ]; then
		ima_signed=1
		if [ $VERBOSE -ne 0 ] ; then
			echo "$TEST: [INFO] kexec kernel image IMA signed"
		fi
	elif [ $VERBOSE -ne 0 ]; then
		echo "$TEST: [INFO] kexec kernel image not IMA signed"
	fi
}

kexec_file_load_test()
{
	succeed_msg="$TEST: kexec_file_load succeeded "
	failed_msg="$TEST: kexec_file_load failed "
	platformkey_msg="try enabling the CONFIG_INTEGRITY_PLATFORM_KEYRING"
	rc=0

	line=$(kexec --load --kexec-file-syscall $KERNEL_IMAGE 2>&1)

	# kexec_file_load succeeded. In secureboot mode with an architecture
	# specific policy, make sure either an IMA or PE signature exists.
	if [ $? -eq 0 ]; then
		kexec --unload --kexec-file-syscall
		if [ $arch_policy -eq 1 ] && [ $ima_signed -eq 0 ] && \
		   [ $pe_signed -eq 0 ]; then
			echo $succeed_msg "(missing sigs) [FAIL]"
			rc=1
		elif [ $ima_sig_required -eq 1 ] && [ $ima_signed -eq 0 ]; then
			echo $succeed_msg "(missing imasig) [FAIL]"
			rc=1
		elif [ $pe_sig_required -eq 1 ] && [ $pe_signed -eq 0 ]; then
			echo $succeed_msg "(missing PE sig) [FAIL]"
			rc=1
		elif [ $ima_read_policy -eq 0 ] && [ $ima_sig_required -eq 0 ] \
		      && [ $ima_signed -eq 0]; then
			echo $succeed_msg "[UNKNOWN]"
		else
			echo $succeed_msg "[PASS]"
		fi
		return $rc
	fi

	# Check the reason for the kexec_file_load failure
	echo $line | grep -q "Required key not available"
	if [ $? -eq 0 ]; then
		rc=1
		if [ $platform_keyring -eq 0 ]; then
			echo $failed_msg "(-ENOKEY)," $platformkey_msg
		else
			echo $failed_msg "(-ENOKEY)"
		fi
	elif [ $ima_sig_required -eq 1 ] && [ $ima_signed -eq 0 ]; then
		echo $TEST: $failed_msg "[PASS]"
	elif [ $pe_sig_required -eq 1 ] && [ $pe_signed -eq 0 ]; then
		echo $TEST: $failed_msg "[PASS]"
	elif [ $ima_read_policy -eq 0 ] && [ $ima_sig_required -eq 0 ] && \
	     [ $ima_signed -eq 0]; then
		echo $failed_msg "[UNKNOWN]"
	else
		echo $TEST: $failed_msg "[FAIL]"
		rc=1
	fi
	return $rc
}

# kexec requires root privileges
if [ $(id -ru) != 0 ]; then
	echo "$TEST: Requires root privileges" >&2
	exit $ksft_skip
fi

check_kconfig_options
check_for_apps
check_runtime
check_for_sigs
kexec_file_load_test
rc=$?
exit $rc
