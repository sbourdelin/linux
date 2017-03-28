CFLAGS += -O2 -g -std=gnu99 -Wall
CFLAGS += -I../../../../usr/include/
CFLAGS += $(shell pkg-config --cflags mount)
LDLIBS += $(shell pkg-config --libs mount)

TEST_PROGS := gpio-mockup.sh
TEST_PROGS_EXTENDED := gpio-mockup-chardev
TEST_FILES := gpio-mockup-sysfs.sh

GPIODIR := ../../../gpio
GPIOOBJ := gpio-utils.o
GPIOINC := gpio.h

include ../lib.mk

all: $(GPIOINC) $(TEST_PROGS_EXTENDED)

clean:
	$(RM) $(TEST_PROGS_EXTENDED)
	$(MAKE) -C $(GPIODIR) clean

$(TEST_PROGS_EXTENDED): $(GPIODIR)/$(GPIOOBJ)

$(GPIODIR)/$(GPIOOBJ):
	$(MAKE) -C $(GPIODIR)

$(GPIOINC):
	$(MAKE) -C ../../../.. headers_install
