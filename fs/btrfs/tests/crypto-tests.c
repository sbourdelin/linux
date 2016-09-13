#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/scatterlist.h>
#include <linux/random.h>
#include <keys/user-type.h>
#include "../extent_io.h"
#include "../encrypt.h"
#include "../hash.h"
#include "crypto-tests.h"

struct page *known_data_page = 0;
char *known_data_str = 0;
struct key *btrfs_key = 0;

int __blkcipher(int encrypt, char *str, size_t sz)
{
	return 0;
}

int __ablkcipher(int enc, char *cipher_name, struct page *page,
						unsigned long len)
{
	struct btrfs_ablkcipher_req_data btrfs_req;
	char *key_str;

	memset(&btrfs_req, 0, sizeof(btrfs_req));
	key_str = kstrdup(
	"\x12\x34\x56\x78\x90\xab\xcd\xef\x12\x34\x56\x78\x90\xab\xcd\xef",
			GFP_NOFS);
	memcpy(btrfs_req.key, key_str, 16);

	strcpy(btrfs_req.cipher_name, cipher_name);
	return btrfs_do_ablkcipher(enc, page, len, &btrfs_req);
}

bool is_same_as_known_data_page(char *a, char *b, size_t sz)
{
	return !memcmp(a, b, sz);
}

void __check_same_print(char *a, char *b, size_t sz, int for_encrypt)
{
	if (is_same_as_known_data_page(a, b, sz)) {
		if (for_encrypt)
			printk("_BTRFS_: encrypt failed !!!\n");
		else
			printk("_BTRFS_: decrypt success\n");
	} else {
		if (for_encrypt)
			printk("_BTRFS_: encrypt success\n");
		else
			printk("_BTRFS_: decrypt failed !!!\n");
	}
}

void test_pr_result(struct page *page_in, int for_encrypt)
{
	char *a = page_address(page_in);
	char *b = page_address(known_data_page);

	__check_same_print(a, b, TEST_DATA_SIZE, for_encrypt);
}

void test_pr_result_str(char *a, int for_encrypt)
{
	__check_same_print(a, known_data_str, TEST_DATA_SIZE, for_encrypt);
}

void test_init(void)
{
	char *kaddr;
	char *str = "deadbeef";
	unsigned long dlen = strlen(str);
	unsigned long offset;

	if (known_data_page)
		return;

	if (TEST_DATA_SIZE > PAGE_SIZE) {
		printk("_BTRFS_: TEST_DATA_PAGE is bigger than PAGE_SIZE\n");
		return;
	}

	known_data_page = alloc_page(GFP_NOFS);
	//known_data_page = get_zeroed_page(GFP_NOFS);
	if (!known_data_page) {
		printk("_BTRFS_: FAILED to alloc page\n");
		return;
	}

	/* Fill known data */
	kaddr = page_address(known_data_page);
	for (offset = 0; offset < TEST_DATA_SIZE; offset = offset + dlen)
		memcpy(kaddr + offset, str, dlen);

	flush_kernel_dcache_page(known_data_page);
}

void test_fini(void)
{
	if (known_data_page)
		__free_page(known_data_page);
}


void test_print_data(const char *str, char *prefix, size_t sz, int print_as_str)
{
	int i;
	printk("_BTRFS_: %s: sz %lu: ", prefix, sz);

	if (print_as_str)
		for (i = 0; i < sz; i++) printk("%c", str[i]);
	else
		for (i = 0; i < sz; i++) printk("%02x ", 0xF & str[i]);

	printk("\n");
}

struct page *test_alloc_page_cpy_known_data(void)
{
	struct page *page;
	char *kaddr;
	char *kaddr_known_data;

	page = alloc_page(GFP_NOFS|__GFP_HIGHMEM);
	if (!page) {
		printk("_BTRFS_: FAILED to alloc page\n");
		return NULL;
	}
	kaddr = kmap(page);

	if (!known_data_page)
		test_init();
	kaddr_known_data = kmap(known_data_page);

	memcpy(kaddr, kaddr_known_data, TEST_DATA_SIZE);

	kunmap(page);
	kunmap(known_data_page);

	return page;
}

char *test_alloc_known_data_str(void)
{
	char *str;

	known_data_str = kzalloc(TEST_DATA_SIZE, GFP_NOFS);
	strncpy(known_data_str, "This is test", TEST_DATA_SIZE);

	str = kzalloc(TEST_DATA_SIZE, GFP_NOFS);
	memcpy(str, known_data_str, TEST_DATA_SIZE);
	return str;
}

void test_blkcipher(void)
{
	int ret;
	char *str;

	str = test_alloc_known_data_str();

	printk("_BTRFS_: ------ testing blkcipher start ------\n");
	ret = __blkcipher(1, str, TEST_DATA_SIZE);
	if (ret) goto out;
	test_pr_result_str(str, 1);
	ret = __blkcipher(0, str, TEST_DATA_SIZE);
	if (ret) goto out;
	test_pr_result_str(str, 0);
	printk("_BTRFS_: ------ testing blkcipher end ------\n");

out:
	kfree(str);
	kfree(known_data_str);
	known_data_str = NULL;
}

void test_ablkcipher(void)
{
	struct page *page;

	test_init();
	page = test_alloc_page_cpy_known_data();

	printk("_BTRFS_: ------- testing ablkcipher start ---------\n");
	__ablkcipher(1, "cts(cbc(aes))", page, TEST_DATA_SIZE);
	test_pr_result(page, 1);
	__ablkcipher(0, "cts(cbc(aes))", page, TEST_DATA_SIZE);
	test_pr_result(page, 0);

	__ablkcipher(1, "ctr(aes)", page, TEST_DATA_SIZE);
	test_pr_result(page, 1);
	__ablkcipher(0, "ctr(aes)", page, TEST_DATA_SIZE);
	test_pr_result(page, 0);
	printk("_BTRFS_: ------ testing ablkcipher end ------------\n\n");

	__free_page(page);

	test_fini();
}

bool does_pages_match(struct address_space *mapping, u64 start, unsigned long len,
			unsigned long nr_page, struct page **pages)
{
	int ret;
	char *in;
	char *out;
	struct page *in_page;
	struct page *out_page;
	unsigned long bytes_left = len;
	unsigned long cur_page_len;
	unsigned long cr_page;

	for (cr_page = 0; cr_page < nr_page; cr_page++) {

		WARN_ON(!bytes_left);

		in_page = find_get_page(mapping, start >> PAGE_SHIFT);
		out_page = pages[cr_page];
		cur_page_len = min(bytes_left, PAGE_SIZE);

		in = kmap(in_page);
		out = kmap(out_page);
		ret = memcmp(out, in, cur_page_len);
		kunmap(out_page);
		kunmap(in_page);
		if (ret)
			return false;

		start += cur_page_len;
		bytes_left = bytes_left - cur_page_len;
	}

	return true;
}

void test_key(char *keytag)
{
	int ret;
	unsigned char key_payload[16];

	printk("_BTRFS_: ---- test_key() start -----\n");
	ret = btrfs_request_key(keytag, key_payload);
	if (ret == -ENOKEY) {
		printk("_BTRFS_: NOKEY: keytag %s\n", keytag);
		return;
	}
	if (ret) {
		printk("_BTRFS_: request key failed !! %d\n", ret);
		return;
	}
	printk("_BTRFS_: ------ test_key() end -----\n");
}

void test_print_data_v2(struct page *page, int endec)
{
	char *data;
	char tmp[80];

	data = kmap(page);
	strncpy(tmp, data, 80);
	kunmap(page);

	printk("_BTRFS_: %s\n", tmp);
}

void test_open_key()
{
	btrfs_key = request_key(&key_type_user, "btrfs_test", NULL);
	if (IS_ERR(btrfs_key)) {
		printk("_BTRFS_: getting test key 'btrfs_test' failed\n");
		btrfs_key = NULL;
		return;
	}

	printk("_BTRFS_: Got test key serial %d\n", btrfs_key->serial);
	down_write_nested(&btrfs_key->sem, 1);
}

void test_close_key()
{
	if (btrfs_key) {
		up_write(&btrfs_key->sem);
		key_put(btrfs_key);
	}
}

int test_ablkciphear2(char *cipher_name, size_t test_size)
{
	u32 crc1 = ~(u32)0;
	u32 crc2 = ~(u32)0;
	u32 crc3 = ~(u32)0;
	u32 seed;
	struct page *page;
	char *kaddr;
	int ret = 0;
	unsigned int page_nr;

	page_nr = test_size/PAGE_SIZE;
	page = alloc_pages(GFP_KERNEL, page_nr);
	if (unlikely(!page)) {
		printk("_BTRFS_: FAILED to alloc page\n");
		return -ENOMEM;
	}
	kaddr = kmap(page);

	get_random_bytes(&seed, 4);
	crc1 = btrfs_crc32c(seed, kaddr, test_size);

	/* Encrypt */
	ret = __ablkcipher(1, cipher_name, page, test_size);
	if (ret) {
		printk("BTRFS_TEST: Encrypt '%s' size '%lu' Failed\n",
			cipher_name, test_size);
		return ret;
	}

	crc2 = btrfs_crc32c(seed, kaddr, test_size);

	/* Decrypt */
	ret = __ablkcipher(0, cipher_name, page, test_size);
	if (ret) {
		printk("BTRFS_TEST: Decrypt '%s' size '%lu' Failed\n",
			cipher_name, test_size);
		return ret;
	}

	crc3 = btrfs_crc32c(seed, kaddr, test_size);

	if (crc1 == crc2) {
		printk("BTRFS_TEST: %u:%u:%u\n", crc1,crc2,crc3);
		printk("!!! BTRFS: ERROR: Encrypt failed !!! \n");
		ret = -EINVAL;
	}
	if (!ret && (crc1 != crc3)) {
		printk("BTRFS_TEST: %u:%u:%u\n", crc1,crc2,crc3);
		printk("!!! BTRFS: ERROR: Decrypt failed !!!\n");
		ret = -EINVAL;
	}

	kunmap(page);
	__free_pages(page, page_nr);

	return ret;
}

void workout(char *cipher_name)
{
	if (test_ablkciphear2(cipher_name, 16))
		return;
	if (test_ablkciphear2(cipher_name, 2024))
		return;
	if (test_ablkciphear2(cipher_name, 4096))
		return;
	if (test_ablkciphear2(cipher_name, 8192))
		return;
	if (test_ablkciphear2(cipher_name, 8333))
		return;

	test_ablkciphear2(cipher_name, 4097);
	test_ablkciphear2(cipher_name, 1);
	test_ablkciphear2(cipher_name, 15);
}

void btrfs_selftest_crypto(void)
{
	char cipher_name[17];

	strcpy(cipher_name, "ctr(aes)");
	workout(cipher_name);
	/*
	strcpy(cipher_name, "cts(cbc(aes))");
	workout(cipher_name);
	*/
}
