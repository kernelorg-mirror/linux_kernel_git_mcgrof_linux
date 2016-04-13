#!/bin/bash

# This performs a series tests against firmware_class to excercise the
# firmware_class driver with focus only on the extensible system data API.
#
# To make this test self contained, and note pollute your distribution
# firmware install paths, we reset the custom load directory to a
# temporary location.

set -e

DIR=/sys/devices/virtual/misc/test_sysdata0/

if [ ! -d $DIR ]; then
	modprobe test_sysdata0
fi

OLD_FWPATH=$(cat /sys/module/firmware_class/parameters/path)

FWPATH=$(mktemp -d)
DEFAULT_SYSDATA="test-sysdata.bin"
FW="$FWPATH/$DEFAULT_SYSDATA"

test_reqs()
{
	if ! which diff 2> /dev/null > /dev/null; then
		echo "$0: You need diff installed"
		exit 1
	fi
}

test_finish()
{
	echo -n "$OLD_PATH" >/sys/module/firmware_class/parameters/path
	rm -f "$FW"
	rmdir "$FWPATH"
}

trap "test_finish" EXIT

# Set the kernel search path.
echo -n "$FWPATH" >/sys/module/firmware_class/parameters/path

# This is an unlikely real-world firmware content. :)
echo "ABCD0123" >"$FW"

NAME=$(basename "$FW")

errno_name_to_val()
{
	case "$1" in
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

config_set_async()
{
	if ! echo -n 1 >$DIR/config_async ; then
		echo "$0: Unable to set to async" >&2
		exit 1
	fi
}

config_disable_async()
{
	if ! echo -n 0 >$DIR/config_async ; then
		echo "$0: Unable to set to sync" >&2
		exit 1
	fi
}

config_set_optional()
{
	if ! echo -n 1 >$DIR/config_optional ; then
		echo "$0: Unable to set to optional" >&2
		exit 1
	fi
}

config_disable_optional()
{
	if ! echo -n 0 >$DIR/config_optional ; then
		echo "$0: Unable to disable optional" >&2
		exit 1
	fi
}

config_set_keep()
{
	if ! echo -n 1 >$DIR/config_keep; then
		echo "$0: Unable to set to keep" >&2
		exit 1
	fi
}

config_disable_keep()
{
	if ! echo -n 0 >$DIR/config_keep; then
		echo "$0: Unable to disable keep option" >&2
		exit 1
	fi
}

config_enable_opt_cb()
{
	if ! echo -n 1 >$DIR/config_enable_opt_cb; then
		echo "$0: Unable to set to optional" >&2
		exit 1
	fi
}

config_disable_opt_cb()
{
	if ! echo -n 0 >$DIR/config_enable_opt_cb; then
		echo "$0: Unable to disable keep option" >&2
		exit 1
	fi
}


# For special characters use printf directly,
# refer to sysdata_test_0001
config_set_name()
{
	if ! echo -n $1 >$DIR/config_name; then
		echo "$0: Unable to set name" >&2
		exit 1
	fi
}

config_get_name()
{
	cat $DIR/config_name
}

# For special characters use printf directly,
# refer to sysdata_test_0001
config_set_default_name()
{
	if ! echo -n $1 >$DIR/config_default_name; then
		echo "$0: Unable to set default_name" >&2
		exit 1
	fi
}

config_get_default_name()
{
	cat $DIR/config_default_name
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
	echo "$1: OK! - loading sysdata"
}

config_trigger_want_fail()
{
	if echo "1" > $DIR/trigger_config 2>/dev/null; then
		echo "$1: FAIL - loading was expected to fail"
		config_show_config
		exit 1
	fi
	echo "$1: OK! - loading failed as expected"
}

config_file_should_match()
{
	FILE=$(config_get_name)
	# On this one we expect the file to exist so leave stderr in
	if ! $(diff -q "$FWPATH"/"$FILE" /dev/test_sysdata0 > /dev/null) > /dev/null; then
		echo "$1: FAIL - file $FILE did not match contents in /dev/test_sysdata0" >&2
		config_show_config
		exit 1
	fi
	echo "$1: OK! - $FILE == /dev/test_sysdata0"
}

config_file_should_match_default()
{
	FILE=$(config_get_default_name)
	# On this one we expect the file to exist so leave stderr in
	if ! $(diff -q "$FWPATH"/"$FILE" /dev/test_sysdata0 > /dev/null) > /dev/null; then
		echo "$1: FAIL - file $FILE did not match contents in /dev/test_sysdata0" >&2
		config_show_config
		exit 1
	fi
	echo "$1: OK! - $FILE == /dev/test_sysdata0"
}

config_file_should_not_match()
{
	FILE=$(config_get_name)
	# File may not exist, so skip those error messages as well
	if $(diff -q $FWPATH/$FILE /dev/test_sysdata0 2> /dev/null) 2> /dev/null ; then
		echo "$1: FAIL - file $FILE was not expected to match /dev/null" >&2
		config_show_config
		exit 1
	fi
	echo "$1: OK! - $FILE != /dev/test_sysdata0"
}

config_default_file_should_match()
{
	FILE=$(config_get_default_name)
	diff -q $FWPATH/$FILE /dev/test_sysdata0 2> /dev/null
	if ! $? ; then
		echo "$1: FAIL - file $FILE expected to match /dev/test_sysdata0" >&2
		config_show_config
		exit 1
	fi
	echo "$1: OK! [file integrity matches]"
}

config_default_file_should_not_match()
{
	FILE=$(config_get_default_name)
	diff -q FWPATH/$FILE /dev/test_sysdata0 2> /dev/null
	if $? 2> /dev/null ; then
		echo "$1: FAIL - file $FILE was not expected to match test_sysdata0" >&2
		config_show_config
		exit 1
	fi
	echo "$1: OK!"
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

sysdata_set_sync_defaults()
{
	config_reset
}

sysdata_set_async_defaults()
{
	config_reset
	config_set_async
}

sysdata_test_0001s()
{
	NAME='\000'

	sysdata_set_sync_defaults
	config_set_name $NAME
	printf '\000' >"$DIR"/config_name
	config_trigger_want_fail ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} -EINVAL
}

sysdata_test_0001a()
{
	NAME='\000'

	sysdata_set_async_defaults
	printf '\000' >"$DIR"/config_name
	config_trigger_want_fail ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} -EINVAL
}

sysdata_test_0001()
{
	sysdata_test_0001s
	sysdata_test_0001a
}

sysdata_test_0002s()
{
	NAME="nope-$DEFAULT_SYSDATA"

	sysdata_set_sync_defaults
	config_set_name ${FUNCNAME[0]}
	config_trigger_want_fail ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} -ENOENT
}

sysdata_test_0002a()
{
	NAME="nope-$DEFAULT_SYSDATA"

	sysdata_set_async_defaults
	config_set_name $NAME
	config_trigger_want_fail ${FUNCNAME[0]}
	# This may seem odd to expect success on a bogus
	# file but remember this is an async call, the actual
	# error handling is managed by the async callbacks.
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

sysdata_test_0002()
{
	#sysdata_test_0002s
	sysdata_test_0002a
}

sysdata_test_0003()
{
	config_reset
	config_file_should_not_match ${FUNCNAME[0]}
}

sysdata_test_0004s()
{
	TEST="sysdata_test_0004s"

	sysdata_set_sync_defaults
	config_trigger ${FUNCNAME[0]}
	config_file_should_match ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

sysdata_test_0004a()
{
	sysdata_set_async_defaults
	config_trigger ${FUNCNAME[0]}
	config_file_should_match ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

sysdata_test_0004()
{
	sysdata_test_0004s
	sysdata_test_0004a
}

sysdata_test_0005s()
{
	NAME="nope-$DEFAULT_SYSDATA"

	sysdata_set_sync_defaults
	config_set_optional
	config_set_name $NAME
	config_trigger_want_fail ${FUNCNAME[0]}
	# We do this to ensure the default backup callback hasn't
	# been called yet
	config_file_should_not_match ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

sysdata_test_0005a()
{
	NAME="nope-$DEFAULT_SYSDATA"

	sysdata_set_async_defaults
	config_set_optional
	config_set_name $NAME
	config_trigger_want_fail ${FUNCNAME[0]}
	# We do this to ensure the default backup callback hasn't
	# been called yet
	config_file_should_not_match ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

sysdata_test_0005()
{
	sysdata_test_0005s
	sysdata_test_0005a
}

sysdata_test_0006s()
{
	sysdata_set_sync_defaults
	config_set_optional
	config_trigger ${FUNCNAME[0]}
	config_file_should_match ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

sysdata_test_0006a()
{
	sysdata_set_async_defaults
	config_set_optional
	config_trigger ${FUNCNAME[0]}
	config_file_should_match ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

sysdata_test_0006()
{
	sysdata_test_0006s
	sysdata_test_0006a
}

sysdata_test_0007s()
{
	sysdata_set_sync_defaults
	config_set_keep
	config_trigger ${FUNCNAME[0]}
	config_file_should_match ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

sysdata_test_0007a()
{
	sysdata_set_async_defaults
	config_set_keep
	config_trigger ${FUNCNAME[0]}
	config_file_should_match ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

sysdata_test_0007()
{
	sysdata_test_0007s
	sysdata_test_0007a
}

sysdata_test_0008s()
{
	NAME="nope-$DEFAULT_SYSDATA"

	sysdata_set_sync_defaults
	config_set_name $NAME
	config_set_optional
	config_enable_opt_cb
	config_trigger ${FUNCNAME[0]}
	config_file_should_match_default ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

sysdata_test_0008a()
{
	NAME="nope-$DEFAULT_SYSDATA"

	sysdata_set_async_defaults
	config_set_name $NAME
	config_set_optional
	config_enable_opt_cb
	config_trigger ${FUNCNAME[0]}
	config_file_should_match_default ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

sysdata_test_0008()
{
	sysdata_test_0008s
	sysdata_test_0008a
}

sysdata_test_0009s()
{
	NAME="nope-$DEFAULT_SYSDATA"

	sysdata_set_sync_defaults
	config_set_name $NAME
	config_set_keep
	config_set_optional
	config_enable_opt_cb
	config_trigger ${FUNCNAME[0]}
	config_file_should_match_default ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

sysdata_test_0009a()
{
	NAME="nope-$DEFAULT_SYSDATA"

	sysdata_set_async_defaults
	config_set_name $NAME
	config_set_keep
	config_set_optional
	config_enable_opt_cb
	config_trigger ${FUNCNAME[0]}
	config_file_should_match_default ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

sysdata_test_0009()
{
	sysdata_test_0009s
	sysdata_test_0009a
}

sysdata_test_0010s()
{
	NAME="nope-$DEFAULT_SYSDATA"

	sysdata_set_sync_defaults
	config_set_name $NAME
	config_set_default_name $NAME
	config_set_keep
	config_set_optional
	config_enable_opt_cb
	config_trigger_want_fail ${FUNCNAME[0]}
	config_file_should_not_match ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} -ENOENT
}

sysdata_test_0010a()
{
	NAME="nope-$DEFAULT_SYSDATA"

	sysdata_set_async_defaults
	config_set_name $NAME
	config_set_default_name $NAME
	config_set_keep
	config_set_optional
	config_enable_opt_cb
	config_trigger_want_fail ${FUNCNAME[0]}
	config_file_should_not_match ${FUNCNAME[0]}
	config_expect_result ${FUNCNAME[0]} SUCCESS
}

sysdata_test_0010()
{
	sysdata_test_0010s
	sysdata_test_0010a
}

test_reqs

usage()
{
	echo "Usage: $0 [ -t <4-number-digit> ]"
	echo Valid tests: 0001-0005
	echo
	echo 0001 - Empty string should be ignored
	echo 0002 - Files that do not exist should be ignored
	echo 0003 - Verify test_sysdata0 has nothing loaded upon reset
	echo 0004 - Simple sync and async loader
	echo 0005 - Verify optional loading is not fatal
	echo 0006 - Verify optional loading enables loading
	echo 0007 - Verify keep works
	echo 0008 - Verify optional callback works
	echo 0009 - Verify optional callback works, keep
	echo 0010 - Verify when fallback file is not present
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

	RUN_TEST=sysdata_test_$2
	$RUN_TEST
	exit 0
fi

sysdata_test_0001
sysdata_test_0002
sysdata_test_0003

exit 0
