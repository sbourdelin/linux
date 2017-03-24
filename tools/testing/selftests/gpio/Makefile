CFLAGS += -O2 -g -std=gnu99 -Wall
CFLAGS += -I../../../../include/uapi/ -I../../../../include/
CFLAGS += $(shell pkg-config --cflags mount)
LDLIBS += $(shell pkg-config --libs mount)

TEST_PROGS := gpio-mockup.sh
TEST_PROGS_EXTENDED := gpio-mockup-chardev
TEST_FILES := gpio-mockup-sysfs.sh

GPIODIR := ../../../gpio
GPIOOBJ := gpio-utils.o

include ../lib.mk

all: $(TEST_PROGS_EXTENDED)

clean:
	$(RM) $(TEST_PROGS_EXTENDED)
	$(MAKE) -C $(GPIODIR) clean

$(TEST_PROGS_EXTENDED): $(GPIODIR)/$(GPIOOBJ)

$(GPIODIR)/$(GPIOOBJ):
	$(MAKE) -C $(GPIODIR)
