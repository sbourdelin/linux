=============================
Key Stream SDIO Device Driver
=============================

Current Status
--------------

Firmware Interface Layer only.
Skeleton implementation in all other files.

Description
-----------

Driver conversion from WEXT interface to cfg80211 API.

The current KeyStream SDIO wireless driver (drivers/staging/ks7010)
implements the WEXT interface.

This driver is based on source code from the Ben Nanonote extra repository [1]
which is based on the original v007 release from Renesas [2].

[1] http://projects.qi-hardware.com/index.php/p/openwrt-packages/source/tree/master/ks7010/src
[2] http://downloads.qi-hardware.com/software/ks7010_sdio_v007.tar.bz2

Extensive refactoring has been done to the driver whilst in staging
and the current mainline tip is untested.

WEXT driver files :-
 - ks7010_sdio.[ch] 	- SDIO code.
 - ks_hostif.[ch] 	- Device interface.
 - ks_wlan_net.c 	- WEXT interface.
 - mic.[ch] 		- Custom Michael MIC implementation.
 - eap_packet.h 	- EAP headers.
 - ks_wlan_ioctl.h 	- WEXT IOCTL.

cfg80211 driver files :-
 - main.c 		- Main driver file (net_device_ops etc).
 - ks7010.h 		- Main driver header file.
 - common.h 		- Constant definitions and forward declarations.
 - eap.h 		- EAPOL structure descriptions.
 - sdio.[ch] 		- SDIO code.
 - fil.[ch] 		- Firmware Interface Layer.
 - fil_types.h 		- Internal FIL types.
 - hif.[ch] 		- Host Interface Layer.
 - cfg80211.c 		- cfg80211 API implementation.
 - tx.c 		- Transmit path functions.

cfg80211 driver files to do :-
 - mic.[ch] 		- Interface to the kernel Michael MIC implementation.
 - rx.c 		- Recive path functions.

Other Information
=================

Hardware
--------
https://wikidevi.com/wiki/Spectec_SDW-821_(KeyStream)
https://wikidevi.com/wiki/Spectec_SDW-823

Kernel Config
-------------
http://cateee.net/lkddb/web-lkddb/KS7010.html

also enable
 - MMC_DEBUG

Testing
-------
http://elinux.org/Tests:SDIO-KS7010

Writing SDIO Linux Drivers
--------------------------
http://www.varsanofiev.com/inside/WritingLinuxSDIODrivers.htm
