#!/bin/bash

BACKEND=$(dirname $BASH_SOURCE)/config-bisect.pl

usage() {
	echo Usage:
	echo "$0 <outputdir> command [args]"
	echo commands:
	echo    reset
	echo    init
	echo    diff
	echo    bad [conf]
	echo    good [conf]
	echo    skip
	echo If conf is unspecified, \".config\" is used.
	echo If cross-compiling, pass the ARCH environment variable.
}

reset() {
	rm -rf $STATE
}

init() {
	reset
	mkdir -p $STATE/bad
	mkdir -p $STATE/good
	echo 0 > $STATE/good/idx
	echo 0 > $STATE/bad/idx
}

next() {
	if ! [ -d $STATE ]; then
		echo $0: No config-bisect in progress -- initializing
		init
	fi

	IDX=$(cat $STATE/$1/idx)
	IDX=$(expr $IDX + 1)

	CONF=$2
	if [ -z "$CONF" ]; then
		CONF=$O/.config
	fi

	cp $CONF $STATE/$1/$IDX
	echo $IDX > $STATE/$1/idx
}

show_diff() {
	GI=$(cat $STATE/good/idx)
	BI=$(cat $STATE/bad/idx)

	if [ $GI != 0 ] && [ $BI != 0 ]; then
		diff -u $STATE/good/$GI $STATE/bad/$BI
	else
		echo $0: cannot diff without at least one good and one bad
	fi
}

genconf() {
	GI=$(cat $STATE/good/idx)
	BI=$(cat $STATE/bad/idx)

	echo good index $GI, bad index $BI

	if [ $GI != 0 ] && [ $BI != 0 ]; then
		$BACKEND $O $STATE/good/$GI $STATE/bad/$BI

		case $? in
		0)
			;;
		2)
			echo Failing config diff:
			show_diff
			;;
		*)
			echo $0: error in backend
			exit 1;
			;;
		esac
	fi
}

if [ -z "$1" -o -z "$2" ]; then
	usage
	exit 1
fi

O=$1
STATE=$O/.config-bisect

case $2 in
init)
	init
	;;
reset)
	reset
	;;
diff)
	show_diff
	;;
bad)
	next bad $3
	genconf
	;;
good)
	next good $3
	genconf
	;;
skip)
	# The options chosen are randomized, so we'll get a different
	# config just by re-running the config bisect backend with the
	# same inputs.
	genconf
	;;
*)
	usage
	exit 1
	;;
esac
