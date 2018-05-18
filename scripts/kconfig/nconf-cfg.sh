#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

PKG="menuw panelw ncursesw"
PKG2="menu panel ncurses"

if pkg-config --exists $PKG; then
	echo libs=\"$(pkg-config --libs $PKG)\"
	exit 0
fi

if pkg-config --exists $PKG2; then
	echo libs=\"$(pkg-config --libs $PKG2)\"
	exit 0
fi

echo >&2 "*"
echo >&2 "* Unable to find the ncurses."
echo >&2 "* Install ncurses (ncurses-devel or libncurses-dev"
echo >&2 "* depending on your distribution)"
echo >&2 "*"
exit 1
