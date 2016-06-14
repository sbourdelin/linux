#!/bin/bash

DIR=$(dirname $(readlink -f $0))
DIR="${DIR}/../"

GLIMPSEINDEX="`which ${GLIMPSEINDEX:=glimpseindex}`"
if [ ! -x "$GLIMPSEINDEX" ]; then
	echo 'glimpseindex can be obtained at https://github.com/mcgrof/glimpse.git'
	exit 1
fi

find $DIR/* -name "*.[ch]" | $GLIMPSEINDEX -o -H . -F
