#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# This runs all known tests across all known possible configurations we could
# emulate in one run.

set -e

TEST_DIR=$(dirname $0)
source $TEST_DIR/fw_lib.sh

run_tests()
{
	$TEST_DIR/fw_filesystem.sh
	$TEST_DIR/fw_fallback.sh
}

run_test_config_0001()
{
	echo "-----------------------------------------------------"
	echo "Running kernel configuration test 1 -- rare"
	echo "Emulates:"
	echo "CONFIG_FW_LOADER=y"
	echo "CONFIG_FW_LOADER_USER_HELPER=n"
	echo "CONFIG_FW_LOADER_USER_HELPER_FALLBACK=n"
	debugfs_set_force_sysfs_fallback N
	debugfs_set_ignore_sysfs_fallback Y
	run_tests
}

run_test_config_0002()
{
	echo "-----------------------------------------------------"
	echo "Running kernel configuration test 2 -- distro"
	echo "Emulates:"
	echo "CONFIG_FW_LOADER=y"
	echo "CONFIG_FW_LOADER_USER_HELPER=y"
	echo "CONFIG_FW_LOADER_USER_HELPER_FALLBACK=n"
	debugfs_set_force_sysfs_fallback N
	debugfs_set_ignore_sysfs_fallback N
	run_tests
}

run_test_config_0003()
{
	echo "-----------------------------------------------------"
	echo "Running kernel configuration test 3 -- android"
	echo "Emulates:"
	echo "CONFIG_FW_LOADER=y"
	echo "CONFIG_FW_LOADER_USER_HELPER=y"
	echo "CONFIG_FW_LOADER_USER_HELPER_FALLBACK=y"
	debugfs_set_force_sysfs_fallback Y
	debugfs_set_ignore_sysfs_fallback N
	run_tests
}

check_mods
check_setup

if [ -f $FW_FORCE_SYSFS_FALLBACK ]; then
	run_test_config_0001
	run_test_config_0002
	run_test_config_0003
else
	echo "Running basic kernel configuration, working with your config"
	run_tests
fi
