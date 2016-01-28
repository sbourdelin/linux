noarg:
	$(MAKE) -C ../

TEST_PROGS := run_mmtests
TEST_FILES := hugetlb_vs_thp_test
TEST_FILES += subpage_prot
TEST_FILES += tempfile
TEST_FILES += hugepage-migration
TEST_FILES += page-migration

all: $(TEST_FILES)

$(TEST_FILES): ../harness.c

include ../../lib.mk

tempfile:
	dd if=/dev/zero of=tempfile bs=64k count=1

clean:
	rm -f $(TEST_FILES)
