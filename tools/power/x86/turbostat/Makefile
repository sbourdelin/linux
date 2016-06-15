CC		= $(CROSS_COMPILE)gcc
BUILD_OUTPUT	:= $(CURDIR)
PREFIX		:= /usr
DESTDIR		:=
INCLUDE		:= -I../../utils

ifeq ("$(origin O)", "command line")
	BUILD_OUTPUT := $(O)
endif

OBJS		:= turbostat.o ../../utils/utils.o

turbostat : $(OBJS)
CFLAGS +=	-Wall
CFLAGS +=	-DMSRHEADER='"../../../../arch/x86/include/asm/msr-index.h"'
CFLAGS +=	$(INCLUDE)

%.o: %.c
	@mkdir -p $(BUILD_OUTPUT)
	$(CC) -c $(CFLAGS) $< -o $(BUILD_OUTPUT)/$@

turbostat: $(OBJS)
	@mkdir -p $(BUILD_OUTPUT)
	$(CC) $(CFLAGS) $? -o $(BUILD_OUTPUT)/$@

.PHONY : clean
clean :
	@rm -f $(BUILD_OUTPUT)/turbostat $(BUILD_OUTPUT)/*.o $(BUILD_OUTPUT)/../../utils/utils.o

install : turbostat
	install -d  $(DESTDIR)$(PREFIX)/bin
	install $(BUILD_OUTPUT)/turbostat $(DESTDIR)$(PREFIX)/bin/turbostat
	install -d  $(DESTDIR)$(PREFIX)/share/man/man8
	install turbostat.8 $(DESTDIR)$(PREFIX)/share/man/man8
