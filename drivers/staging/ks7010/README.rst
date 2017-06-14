=============================
Key Stream SDIO Device Driver
=============================

Current Status
--------------

- Firmware Interface Layer complete.
- Host Interface Layer, partial skeleton implementation.
- cfg80211 minimal implementation.
- SDIO mostly implemented.

See drivers/staging/ks7010/TODO.rst for TODO's.

Notes
-----

Some enum mib_attribute identifiers have been renamed. This reduces
the continuity between the WEXT driver and the cfg80211 driver. To
combat this, new identifiers are prefixed with 'MIB_' (instead of
'DOT11_' or 'LOCAL_').

We need to add readiness checks to a lot of functions, check vif is ready (cfg80211),
check sdio function is enabled (sdio), check ks7010 device state (all).

Description
-----------

Driver conversion from WEXT interface to cfg80211 API.

The original KeyStream SDIO wireless driver brought into staging
implements the WEXT interface.

The WEXT driver is based on source code from the Ben Nanonote extra repository [1]
which is based on the original v007 release from Renesas [2].

[1] http://projects.qi-hardware.com/index.php/p/openwrt-packages/source/tree/master/ks7010/src
[2] http://downloads.qi-hardware.com/software/ks7010_sdio_v007.tar.bz2

Extensive refactoring has been done to the WEXT driver whilst in staging
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
 - rx.c 		- Recive path functions.

cfg80211 driver files to do :-
 - mic.[ch] 		- Interface to the kernel Michael MIC implementation.

WEXT driver code now resides in the sub directory drivers/staging/ks7010/wext
while the cfg80211 driver has the 'root' directory (drivers/staging/ks7010).

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
