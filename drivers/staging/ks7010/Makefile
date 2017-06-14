#-------------------------------------------------------------------------
# Driver for KeyStream wireless LAN cards.
#
# Copyright (C) 2005-2008 KeyStream Corp.
# Copyright (C) 2009 Renesas Technology Corp.
# Copyright (C) 2017 Tobin C. Harding.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#-------------------------------------------------------------------------

ccflags-y 	     += -DDEBUG

obj-$(CONFIG_KS7010) += ks7010.o
ks7010-y	     += main.o
ks7010-y	     += tx.o
ks7010-y	     += rx.o
ks7010-y	     += sdio.o
ks7010-y	     += cfg80211.o
ks7010-y	     += fil.o
ks7010-y	     += hif.o
