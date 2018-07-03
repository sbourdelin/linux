#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Check whether kernel changes compile with multiple configurations.
# Configurations that fail for the current commit are tested on an upstream
# commit, and failures are ignored in case the configuration fails on the
# upstream commit as well.
#
# Usage:
# ./scripts/check_build.sh [upstream-commit]
#
# Example:
# ./scripts/check_build.sh origin/master

# Fail early on error or uninitialized values
set -e -u

# Write all files to this directory
declare -r LOG_DIR=build_check_logs

# Gather all configurations we should build, in form of make targets
collect_configurations ()
{
	local CONFIGS="randconfig allyesconfig allnoconfig allmodconfig"
	local -i I=0

	while [ "$I" -le 8 ]; do I=$((I+1)); CONFIGS+=" randconfig"; done

	echo "$CONFIGS"
}

# Build the kernel with the current configuration, return the status, log to $1
build ()
{
	local -r LOG_FILE="$1"
	local -i STATUS=0

	make clean -j $(nproc) &> /dev/null
	make -j $(nproc) &>> "$LOG_FILE" || STATUS=$?

	echo "build status: $STATUS" >> "$LOG_FILE"
	echo "[$SECONDS] build status: $STATUS"
	return "$STATUS"
}

# Check whether we're executing from a clean repository, and proper location
check_environment()
{
	local -r UPSTREAM_COMMIT="$1"

	if [ ! -x scripts/check_build.sh ]; then
		echo "error: please execute script from repository root!"
		exit 1
	fi

	if ! git diff-index --quiet HEAD --; then
		echo "error: please commit or stash your local changes!"
		exit 1
	fi

	if ! git cat-file -e "$UPSTREAM_COMMIT" &> /dev/null; then
		echo "error: upstream commit $UPSTREAM_COMMIT does not exist!"
		exit 1
	fi
}

TEST_COMMIT=$(git rev-parse --short HEAD)

LAST_TAG=$(git describe --abbrev=0)
DEFAULT_UPSTREAM="origin/master"
[ -z "$LAST_TAG" ] || DEFAULT_UPSTREAM="$LAST_TAG"
UPSTREAM_COMMIT="${1:-$DEFAULT_UPSTREAM}"

# Before starting, check call location and git environment
check_environment "$UPSTREAM_COMMIT"

# Collect status, as well as failing configuration file names
declare -i OVERALL_STATUS=0
mkdir -p "$LOG_DIR"
BRANCH_FAILS=""
UPSTREAM_FAILS=""

# Build multiple configurations
declare -i COUNT=0
declare -r CONFIGS=$(collect_configurations)
echo "[$SECONDS] Check commit $TEST_COMMIT with upstream reference $UPSTREAM_COMMIT"
for config in $CONFIGS
do
	COUNT=$((COUNT+1))
	echo "[$SECONDS] build $config as build nr $COUNT"
	LOG_FILE=$LOG_DIR/build-"$COUNT"-"$config".log

	# Prepare configuration, and memorize it
	STATUS=0
	make "$config" &> "$LOG_FILE" || STATUS=$?
	cp .config $LOG_DIR/config-"$COUNT"-"$config"

	# Try to build with current configuration
	build "$LOG_FILE" || STATUS=$?

	# Evaluate build failure
	if [ "$STATUS" -ne 0 ]
	then
		cp .config "$LOG_DIR"/config-failing-branch-"$COUNT"-"$config"
		echo "[$SECONDS] check whether current configuration fails upstream as well ..."
		echo "[$SECONDS] go back in time to commit $UPSTREAM_COMMIT"

		# Check whether upstream commit builds
		git checkout "$UPSTREAM_COMMIT" &>> "$LOG_FILE"
		UPSTREAM_STATUS=0
		build "$LOG_FILE" || UPSTREAM_STATUS=$?
		git checkout "$TEST_COMMIT" &>> "$LOG_FILE"

		if [ "$UPSTREAM_STATUS" -ne 0 ]
		then
			echo "[$SECONDS] upstream build fails as well, ignore failure"
			cp .config "$LOG_DIR"/config-failing-upstream-"$COUNT"-"$config"
			UPSTREAM_FAILS+=" config-$COUNT-$config"
		else
			echo "[$SECONDS] upstream build succeeds, while branch build fails"
			BRANCH_FAILS+=" config-$COUNT-$config"
			OVERALL_STATUS=1
		fi

		echo "[$SECONDS] jump back to commit under test, $TEST_COMMIT"
	fi
done

# Print summary, and exit with status of local changes
echo "Finished after $SECONDS seconds"
echo "Found upstream failures: $UPSTREAM_FAILS"
echo "Found branch failures: $BRANCH_FAILS"
echo "Collected configurations and logs can be found in $LOG_DIR"

# Exit 0, if no build fails, or the upstream commit always fails as well
exit $OVERALL_STATUS
