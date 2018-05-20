#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

PKG="ncursesw"
PKG2="ncurses"

if pkg-config --exists $PKG; then
	echo cflags=\"-DNCURSES_WIDECHAR=1 $(pkg-config --cflags $PKG)\"
	echo libs=\"$(pkg-config --libs $PKG)\"
	exit 0
fi

if pkg-config --exists $PKG2; then
	echo cflags=\"$(pkg-config --cflags $PKG2)\"
	echo libs=\"$(pkg-config --libs $PKG2)\"
	exit 0
fi

# Unfortunately, some distributions (e.g. openSUSE) cannot find ncurses
# by pkg-config.
if [ -f /usr/include/ncursesw/ncurses.h ]; then
	echo cflags=\"-DNCURSES_WIDECHAR=1 -I/usr/include/ncursesw\"
	echo libs=\"-lncursesw\"
	exit 0
fi

if [ -f /usr/include/ncurses.h ]; then
	echo cflags=\"\"
	echo libs=\"-lncurses\"
	exit 0
fi

echo >&2 "*"
echo >&2 "* Unable to find the ncurses."
echo >&2 "* Install ncurses (ncurses-devel or libncurses-dev"
echo >&2 "* depending on your distribution)"
echo >&2 "*"
exit 1
