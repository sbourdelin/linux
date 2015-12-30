libusbiplib_la_CPPFLAGS = -DUSBIDS_FILE='"@USBIDS_DIR@/usb.ids"'
libusbiplib_la_CFLAGS   = @EXTRA_CFLAGS@
libusbiplib_la_LDFLAGS  = -version-info @LIBUSBIP_VERSION@

lib_LTLIBRARIES := libusbiplib.la
libusbiplib_la_SOURCES := names.c names.h usbip_host_driver.c usbip_host_driver.h \
		       usbip_common.c usbip_common.h vhci_driver.c vhci_driver.h \
		       usbip_ux.c usbip_ux.h \
		       sysfs_utils.c sysfs_utils.h
