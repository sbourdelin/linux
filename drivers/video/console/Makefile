# Makefile for the Linux graphics to console drivers.
# 5 Aug 1999, James Simmons, <mailto:jsimmons@users.sf.net>
# Rewritten to use lists instead of if-statements.

obj-$(CONFIG_DUMMY_CONSOLE)       += dummycon.o
obj-$(CONFIG_SGI_NEWPORT_CONSOLE) += newport_con.o
obj-$(CONFIG_STI_CONSOLE)         += sticon.o sticore.o
obj-$(CONFIG_VGA_CONSOLE)         += vgacon.o
obj-$(CONFIG_MDA_CONSOLE)         += mdacon.o
ifeq ($(CONFIG_FRAMEBUFFER_CONSOLE),y)
obj-$(CONFIG_FB) += fbcon.o bitblit.o softcursor.o
ifeq ($(CONFIG_FB_TILEBLITTING),y)
obj-$(CONFIG_FB)     += tileblit.o
endif
ifeq ($(CONFIG_FRAMEBUFFER_CONSOLE_ROTATION),y)
obj-$(CONFIG_FB)     += fbcon_rotate.o fbcon_cw.o fbcon_ud.o \
                                         fbcon_ccw.o
endif
endif

obj-$(CONFIG_FB_STI)              += sticore.o
