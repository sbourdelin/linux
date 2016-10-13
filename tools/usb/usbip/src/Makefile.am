AM_CPPFLAGS = -I$(top_srcdir)/libsrc -DUSBIDS_FILE='"@USBIDS_DIR@/usb.ids"'
AM_CFLAGS   = @EXTRA_CFLAGS@
LDADD       = $(top_builddir)/libsrc/libusbip.la

sbin_PROGRAMS := usbip usbipd usbipa

usbip_SOURCES := usbip.h utils.h usbip.c utils.c usbip_network.c \
		 usbip_attach.c usbip_detach.c usbip_list.c \
		 usbip_bind.c usbip_unbind.c usbip_port.c \
		 usbip_connect.c usbip_disconnect.c
usbip_CFLAGS := $(AM_CFLAGS)

usbipd_SOURCES := usbip_network.h usbipd.c usbipd_dev.c usbip_network.c
usbipd_CFLAGS := $(AM_CFLAGS)

usbipa_SOURCES := usbip_network.h usbipd.c usbipd_app.c usbip_network.c
usbipa_CFLAGS := $(AM_CFLAGS) -DUSBIP_DAEMON_APP
