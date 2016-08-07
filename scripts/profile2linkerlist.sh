#!/bin/sh

# Takes a (sorted) output of readprofile and turns it into a list suitable for
# linker scripts.
#
# usage:
#         readprofile | sort -rn | ./profile2linkerlist.sh > functionlist

# Exit on error
set -e

while read LINE; do
	LINE=$( echo $LINE | sed -e s"/[0-9.][0-9.]*//" -e s"/[0-9.][0-9.]*$//" | xargs)
	case $LINE in *unknown*|*total* ) continue
	;;
	esac
	echo '*(.text.'$LINE')'
done
