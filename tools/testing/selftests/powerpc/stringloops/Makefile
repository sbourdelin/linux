# SPDX-License-Identifier: GPL-2.0
# The loops are all 64-bit code
CFLAGS += -I$(CURDIR)

EXTRA_SOURCES := ../harness.c

$(OUTPUT)/memcmp_64: memcmp.c
$(OUTPUT)/memcmp_64: CFLAGS += -m64 -maltivec

$(OUTPUT)/memcmp_32: memcmp.c
$(OUTPUT)/memcmp_32: CFLAGS += -m32

$(OUTPUT)/strlen: strlen.c string.o
$(OUTPUT)/string.o: string.c

ASFLAGS = $(CFLAGS)

TEST_GEN_PROGS := memcmp_32 memcmp_64 strlen

include ../../lib.mk

$(TEST_GEN_PROGS): $(EXTRA_SOURCES)
