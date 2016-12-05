#!/bin/bash
#
# Copyright (C) 2016 Luis R. Rodriguez <mcgrof@kernel.org>
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of copyleft-next (version 0.3.1 or later) as published
# at http://copyleft-next.org/.

# This is a stress test script for kmod, the kernel module loader. It uses
# test_kmod which exposes a series of knobs for the API for us so we can
# tweak each test in userspace rather than in kernelspace.
#
# The way kmod works is it uses the kernel's usermode helper API to eventually
# call /sbin/modprobe. It has a limit of the number of concurrent calls
# possible. The kernel interface to load modules is request_module(), however
# mount uses get_fs_type(). Both behave slightly differently, but the
# differences are important enough to test each call separately. For this
# reason test_kmod starts by providing tests for both calls.
#
# The test driver test_kmod assumes a series of defaults which you can
# override by exporting to your environment prior running this script.
# For instance this script assumes you do not have xfs loaded upon boot.
# If this is false, export DEFAULT_KMOD_FS="ext4" prior to running this
# script if the filesyste module you don't have loaded upon bootup
# is ext4 instead. Refer to allow_user_defaults() for a list of user
# override variables possible.
#
# You'll want at least 4096 GiB of RAM to expect to run these tests
# without running out of memory on them. For other requirements refer
# to test_reqs()

set -e

TEST_DRIVER="test_kmod"

function allow_user_defaults()
{
	if [ -z $DEFAULT_KMOD_DRIVER ]; then
		DEFAULT_KMOD_DRIVER="test_module"
	fi

	if [ -z $DEFAULT_KMOD_FS ]; then
		DEFAULT_KMOD_FS="xfs"
	fi

	if [ -z $PROC_DIR ]; then
		PROC_DIR="/proc/sys/kernel/"
	fi

	if [ -z $MODPROBE_LIMIT ]; then
		MODPROBE_LIMIT=50
	fi

	if [ -z $DIR ]; then
		DIR="/sys/devices/virtual/misc/${TEST_DRIVER}0/"
	fi

	MODPROBE_LIMIT_FILE="${PROC_DIR}/kmod-limit"
}

test_reqs()
{
	if ! which modprobe 2> /dev/null > /dev/null; then
		echo "$0: You need modprobe installed"
		exit 1
	fi

	if ! which kmod 2> /dev/null > /dev/null; then
		echo "$0: You need kmod installed"
		exit 1
	fi

	# kmod 19 has a bad bug where it returns 0 when modprobe
	# gets called *even* if the module was not loaded due to
	# some bad heuristics. For details see:
	#
	# A work around is possible in-kernel but its rather
	# complex.
	KMOD_VERSION=$(kmod --version | awk '{print $3}')
	if [[ $KMOD_VERSION  -le 19 ]]; then
		echo "$0: You need at least kmod 20"
		echo "kmod <= 19 is buggy, for details see:"
		echo "http://git.kernel.org/cgit/utils/kernel/kmod/kmod.git/commit/libkmod/libkmod-module.c?id=fd44a98ae2eb5eb32161088954ab21e58e19dfc4"
		exit 1
	fi
}

function load_req_mod()
{
	if [ ! -d $DIR ]; then
		# Alanis: "Oh isn't it ironic?"
		modprobe $TEST_DRIVER
		if [ ! -d $DIR ]; then
			echo "$0: $DIR not present"
			echo "You must have the following enabled in your kernel:"
			cat $PWD/config
			exit 1
		fi
	fi
}

test_finish()
{
	echo "Test completed"
}

errno_name_to_val()
{
	case "$1" in
	# kmod calls modprobe and upon of a module not found
	# modprobe returns just 1... However in the kernel we
	# *sometimes* see 256...
	MODULE_NOT_FOUND)
		echo 256;;
	SUCCESS)
		echo 0;;
	-EPERM)
		echo -1;;
	-ENOENT)
		echo -2;;
	-EINVAL)
		echo -22;;
	-ERR_ANY)
		echo -123456;;
	*)
		echo invalid;;
	esac
}

errno_val_to_name()
	case "$1" in
	256)
		echo MODULE_NOT_FOUND;;
	0)
		echo SUCCESS;;
	-1)
		echo -EPERM;;
	-2)
		echo -ENOENT;;
	-22)
		echo -EINVAL;;
	-123456)
		echo -ERR_ANY;;
	*)
		echo invalid;;
	esac

config_set_test_case_driver()
{
	if ! echo -n 1 >$DIR/config_test_case; then
		echo "$0: Unable to set to test case to driver" >&2
		exit 1
	fi
}

config_set_test_case_fs()
{
	if ! echo -n 2 >$DIR/config_test_case; then
		echo "$0: Unable to set to test case to fs" >&2
		exit 1
	fi
}

config_num_threads()
{
	if ! echo -n $1 >$DIR/config_num_threads; then
		echo "$0: Unable to set to number of threads" >&2
		exit 1
	fi
}

config_get_modprobe_limit()
{
	if [[ -f ${MODPROBE_LIMIT_FILE} ]] ; then
		MODPROBE_LIMIT=$(cat $MODPROBE_LIMIT_FILE)
	fi
	echo $MODPROBE_LIMIT
}

config_num_thread_limit_extra()
{
	MODPROBE_LIMIT=$(config_get_modprobe_limit)
	let EXTRA_LIMIT=$MODPROBE_LIMIT+$1
	config_num_threads $EXTRA_LIMIT
}

# For special characters use printf directly,
# refer to kmod_test_0001
config_set_driver()
{
	if ! echo -n $1 >$DIR/config_test_driver; then
		echo "$0: Unable to set driver" >&2
		exit 1
	fi
}

config_set_fs()
{
	if ! echo -n $1 >$DIR/config_test_fs; then
		echo "$0: Unable to set driver" >&2
		exit 1
	fi
}

config_get_driver()
{
	cat $DIR/config_test_driver
}

config_get_test_result()
{
	cat $DIR/test_result
}

config_reset()
{
	if ! echo -n "1" >"$DIR"/reset; then
		echo "$0: reset shuld have worked" >&2
		exit 1
	fi
}

config_show_config()
{
	echo "----------------------------------------------------"
	cat "$DIR"/config
	echo "----------------------------------------------------"
}

config_trigger()
{
	if ! echo -n "1" >"$DIR"/trigger_config 2>/dev/null; then
		echo "$1: FAIL - loading should have worked"
		config_show_config
		exit 1
	fi
	echo "$1: OK! - loading kmod test"
}

config_trigger_want_fail()
{
	if echo "1" > $DIR/trigger_config 2>/dev/null; then
		echo "$1: FAIL - test case was expected to fail"
		config_show_config
		exit 1
	fi
	echo "$1: OK! - kmod test case failed as expected"
}

config_expect_result()
{
	RC=$(config_get_test_result)
	RC_NAME=$(errno_val_to_name $RC)

	ERRNO_NAME=$2
	ERRNO=$(errno_name_to_val $ERRNO_NAME)

	if [[ $ERRNO_NAME = "-ERR_ANY" ]]; then
		if [[ $RC -ge 0 ]]; then
			echo "$1: FAIL, test expects $ERRNO_NAME - got $RC_NAME ($RC)" >&2
			config_show_config
			exit 1
		fi
	elif [[ $RC != $ERRNO ]]; then
		echo "$1: FAIL, test expects $ERRNO_NAME ($ERRNO) - got $RC_NAME ($RC)" >&2
		config_show_config
		exit 1
	fi
	echo "$1: OK! - Return value: $RC ($RC_NAME), expected $ERRNO_NAME"
}

kmod_defaults_driver()
{
	config_reset
	modprobe -r $DEFAULT_KMOD_DRIVER
	config_set_driver $DEFAULT_KMOD_DRIVER
}

kmod_defaults_fs()
{
	config_reset
	modprobe -r $DEFAULT_KMOD_FS
	config_set_fs $DEFAULT_KMOD_FS
	config_set_test_case_fs
}

kmod_test_0001_driver()
{
	NAME='\000'

	kmod_defaults_driver
	config_num_threads 1
	printf '\000' >"$DIR"/config_test_driver
	config_trigger ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} MODULE_NOT_FOUND
}

kmod_test_0001_fs()
{
	NAME='\000'

	kmod_defaults_fs
	config_num_threads 1
	printf '\000' >"$DIR"/config_test_fs
	config_trigger ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} -EINVAL
}

kmod_test_0001()
{
	kmod_test_0001_driver
	kmod_test_0001_fs
}

kmod_test_0002_driver()
{
	NAME="nope-$DEFAULT_KMOD_DRIVER"

	kmod_defaults_driver
	config_set_driver $NAME
	config_num_threads 1
	config_trigger ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} MODULE_NOT_FOUND
}

kmod_test_0002_fs()
{
	NAME="nope-$DEFAULT_KMOD_FS"

	kmod_defaults_fs
	config_set_fs $NAME
	config_trigger ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} -EINVAL
}

kmod_test_0002()
{
	kmod_test_0002_driver
	kmod_test_0002_fs
}

kmod_test_0003()
{
	kmod_defaults_fs
	config_num_threads 1
	config_trigger ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

kmod_test_0004()
{
	kmod_defaults_fs
	config_num_threads 2
	config_trigger ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

kmod_test_0005()
{
	kmod_defaults_driver
	config_trigger ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

kmod_test_0006()
{
	kmod_defaults_fs
	config_trigger ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

kmod_test_0007()
{
	kmod_test_0005
	kmod_test_0006
}

kmod_test_0008()
{
	kmod_defaults_driver
	MODPROBE_LIMIT=$(config_get_modprobe_limit)
	let EXTRA=$MODPROBE_LIMIT/2
	config_num_thread_limit_extra $EXTRA
	config_trigger ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

kmod_test_0009()
{
	kmod_defaults_fs
	#MODPROBE_LIMIT=$(config_get_modprobe_limit)
	#let EXTRA=$MODPROBE_LIMIT/3
	config_num_thread_limit_extra 5
	config_trigger ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

trap "test_finish" EXIT
test_reqs
allow_user_defaults
load_req_mod

usage()
{
	echo "Usage: $0 [ -t <4-number-digit> ]"
	echo "Valid tests: 0001-0011"
	echo
	echo "0001 - Simple test - 1 thread  for empty string"
	echo "0002 - Simple test - 1 thread  for modules/filesystems that do not exist"
	echo "0003 - Simple test - 1 thread  for get_fs_type() only"
	echo "0004 - Simple test - 2 threads for get_fs_type() only"
	echo "0005 - multithreaded tests with default setup - request_module() only"
	echo "0006 - multithreaded tests with default setup - get_fs_type() only"
	echo "0007 - multithreaded tests with default setup test request_module() and get_fs_type()"
	echo "0008 - multithreaded - push kmod_concurrent over max_modprobes for request_module()"
	echo "0009 - multithreaded - push kmod_concurrent over max_modprobes for get_fs_type()"
	exit 1
}

# You can ask for a specific test:
if [[ $# > 0 ]] ; then
	if [[ $1 != "-t" ]]; then
		usage
	fi

	re='^[0-9]+$'
	if ! [[ $2 =~ $re ]]; then
		usage
	fi

	RUN_TEST=kmod_test_$2
	$RUN_TEST
	exit 0
fi

# Once tese are enabled please leave them as-is. Write your own test,
# we have tons of space.
kmod_test_0001
kmod_test_0002
kmod_test_0003
kmod_test_0004
kmod_test_0005
kmod_test_0006
kmod_test_0007
kmod_test_0008
kmod_test_0009

exit 0
