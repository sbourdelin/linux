
INCLUDEDIR := -I. -I../../../../../drivers/staging/android/uapi/ -I../../../../../usr/include/
CFLAGS := $(CFLAGS) $(INCLUDEDIR) -Wall -O2 -g

TEST_GEN_FILES := ionapp_export ionapp_import ionmap_test

all: ../../../../../usr/include/linux/dma-buf.h $(TEST_GEN_FILES)

../../../../../usr/include/linux/dma-buf.h:
	make -C ../../../../../ headers_install INSTALL_HDR_PATH=$(shell pwd)/../../../../../usr

$(TEST_GEN_FILES): ipcsocket.c ionutils.c

TEST_PROGS := ion_test.sh

include ../../lib.mk

$(OUTPUT)/ionapp_export: ionapp_export.c ipcsocket.c ionutils.c
$(OUTPUT)/ionapp_import: ionapp_import.c ipcsocket.c ionutils.c
$(OUTPUT)/ionmap_test: ionmap_test.c ionutils.c
