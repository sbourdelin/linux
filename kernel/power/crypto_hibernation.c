// SPDX-License-Identifier: GPL-2.0
/*
 * linux/kernel/power/crypto_hibernation.c
 *
 * This file provides in-kernel encrypted hibernation support.
 *
 * Copyright (c) 2018, Intel Corporation.
 * Copyright (c) 2018, Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 * Copyright (c) 2018, Chen Yu <yu.c.chen@intel.com>
 *
 * Basically, this solution encrypts the pages before they go to
 * the block device, the procedure is illustrated below:
 * 1. The user space reads the salt from the kernel, generates
 *    a symmetrical (AES)key, the kernel uses that key to encrypt the
 *    hibernation image.
 * 2. The salt is saved in image header and passed to
 *    the restore kernel.
 * 3. During restore, the userspace needs to read the salt
 *    from the kernel and probe passphrase from the user
 *    to generate the key and pass that key back to kernel.
 * 4. The restore kernel uses that key to decrypt the image.
 *
 * Generally the advantage is: Users DO NOT have to
 * encrypt the whole swap partition as other tools.
 * After all, ideally kernel memory should be encrypted
 * by the kernel itself.
 */
#define pr_fmt(fmt) "PM: " fmt

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/scatterlist.h>
#include <linux/random.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/cdev.h>
#include <crypto/skcipher.h>
#include <crypto/akcipher.h>
#include <crypto/aes.h>
#include <crypto/hash.h>
#include <crypto/sha.h>
#include <linux/major.h>
#include "power.h"

static int crypto_data(const char *inbuf,
			    int inlen,
			    char *outbuf,
			    int outlen,
			    bool encrypt,
			    int page_idx);
static void crypto_save(void *buf);
static void crypto_restore(void *buf);
static int crypto_init(bool suspend);

/* help function hooks */
static struct hibernation_crypto hib_crypto = {
	.crypto_data = crypto_data,
	.save = crypto_save,
	.restore = crypto_restore,
	.init = crypto_init,
};

/* return the key value. */
static char *get_key_ptr(void)
{
	return hib_crypto.keys.derived_key;
}

/* return the salt value. */
static char *get_salt_ptr(void)
{
	return hib_crypto.keys.salt;
}

/**
 * crypto_data() - en/decrypt the data
 * @inbuf: the source buffer
 * @inlen: the length of source buffer
 * @outbuf: the dest buffer
 * @outlen: the length of dest buffer
 * @encrypt: encrypt or decrypt
 * @page_idx: the index of that page been manipulated
 *
 * Return: 0 on success, non-zero for other cases.
 *
 * Better use SKCIPHER_REQUEST_ON_STACK to support multi-thread
 * encryption, however hibernation does not support multi-threaded
 * swap page write out due to the fact that the swap_map has to be
 * accessed sequently.
 */
static int crypto_data(const char *inbuf,
			    int inlen,
			    char *outbuf,
			    int outlen,
			    bool encrypt,
			    int page_idx)
{
	struct scatterlist src, dst;
	int ret;
	struct {
		__le64 idx;
		u8 padding[HIBERNATE_IV_SIZE - sizeof(__le64)];
	} iv;

	iv.idx = cpu_to_le64(page_idx);
	memset(iv.padding, 0, sizeof(iv.padding));

	/*
	 * Do a AES-256 encryption on every page-index
	 * to generate the IV.
	 */
	crypto_cipher_encrypt_one(hib_crypto.essiv_tfm, (u8 *)&iv,
								(u8 *)&iv);
	sg_init_one(&src, inbuf, inlen);
	sg_init_one(&dst, outbuf, outlen);
	skcipher_request_set_crypt(hib_crypto.req_sk,
				   &src, &dst, outlen, &iv);

	if (encrypt)
		ret = crypto_skcipher_encrypt(hib_crypto.req_sk);
	else
		ret = crypto_skcipher_decrypt(hib_crypto.req_sk);
	if (ret)
		pr_err("%s %scrypt failed: %d\n", __func__,
		       encrypt ? "en" : "de", ret);

	return ret;
}

/* Invoked across hibernate/restore. */
static void crypto_save(void *buf)
{
	memcpy(buf, get_salt_ptr(), HIBERNATE_SALT_BYTES);
}

static void crypto_restore(void *buf)
{
	memcpy(get_salt_ptr(), buf, HIBERNATE_SALT_BYTES);
}

static int init_crypto_helper(void)
{
	int ret = 0;

	/* Symmetric encryption initialization. */
	if (!hib_crypto.tfm_sk) {
		hib_crypto.tfm_sk =
			crypto_alloc_skcipher("xts(aes)", 0, CRYPTO_ALG_ASYNC);
		if (IS_ERR(hib_crypto.tfm_sk)) {
			pr_err("Failed to load transform for aes: %ld\n",
				PTR_ERR(hib_crypto.tfm_sk));
			return -ENOMEM;
		}
	}

	if (!hib_crypto.req_sk) {
		hib_crypto.req_sk =
			skcipher_request_alloc(hib_crypto.tfm_sk, GFP_KERNEL);
		if (!hib_crypto.req_sk) {
			pr_err("Failed to allocate request\n");
			ret = -ENOMEM;
			goto free_tfm_sk;
		}
	}
	skcipher_request_set_callback(hib_crypto.req_sk, 0, NULL, NULL);

	/* Switch to the image key, and prepare for page en/decryption. */
	ret = crypto_skcipher_setkey(hib_crypto.tfm_sk, get_key_ptr(),
				     HIBERNATE_KEY_BYTES);
	if (ret) {
		pr_err("Failed to set the image key. (%d)\n", ret);
		goto free_req_sk;
	}

	return 0;

 free_req_sk:
	skcipher_request_free(hib_crypto.req_sk);
	hib_crypto.req_sk = NULL;
 free_tfm_sk:
	crypto_free_skcipher(hib_crypto.tfm_sk);
	hib_crypto.tfm_sk = NULL;
	return ret;
}

static void exit_crypto_helper(void)
{
	crypto_free_skcipher(hib_crypto.tfm_sk);
	hib_crypto.tfm_sk = NULL;
	skcipher_request_free(hib_crypto.req_sk);
	hib_crypto.req_sk = NULL;
}

/*
 * Copied from init_essiv_generator().
 * Using SHA256 to derive the key and
 * save it.
 */
static int init_iv_generator(const u8 *raw_key, int keysize)
{
	int ret = -EINVAL;
	u8 salt[SHA256_DIGEST_SIZE];

	/* 1. IV generator initialization. */
	if (!hib_crypto.essiv_hash_tfm) {
		hib_crypto.essiv_hash_tfm = crypto_alloc_shash("sha256", 0, 0);
		if (IS_ERR(hib_crypto.essiv_hash_tfm)) {
			pr_err("crypto_hibernate: error allocating SHA-256 transform for IV: %ld\n",
					    PTR_ERR(hib_crypto.essiv_hash_tfm));
			return -ENOMEM;
		}
	}

	if (!hib_crypto.essiv_tfm) {
		hib_crypto.essiv_tfm = crypto_alloc_cipher("aes", 0, 0);
		if (IS_ERR(hib_crypto.essiv_tfm)) {
			pr_err("crypto_hibernate: error allocating cipher aes for IV generation: %ld\n",
					PTR_ERR(hib_crypto.essiv_tfm));
			ret = -ENOMEM;
			goto free_essiv_hash;
		}
	}

	{
		/* 2. Using hash to generate the 256bits AES key */
		SHASH_DESC_ON_STACK(desc, hib_crypto.essiv_hash_tfm);

		desc->tfm = hib_crypto.essiv_hash_tfm;
		desc->flags = 0;
		ret = crypto_shash_digest(desc, raw_key, keysize, salt);
		if (ret) {
			pr_err("crypto_hibernate: error get digest for raw_key\n");
			goto free_essiv_hash;
		}
	}
	/* 3. Switch to the 256bits AES key for later IV generation. */
	ret = crypto_cipher_setkey(hib_crypto.essiv_tfm, salt, sizeof(salt));

 free_essiv_hash:
	crypto_free_shash(hib_crypto.essiv_hash_tfm);
	hib_crypto.essiv_hash_tfm = NULL;
	return ret;
}

/*
 * Either invoked during hibernate or restore.
 */
static int crypto_init(bool suspend)
{
	int ret = 0;

	pr_info("Prepared to %scrypt the image data.\n",
		  suspend ? "en" : "de");
	if (!hib_crypto.keys.user_key_valid) {
		pr_err("Need to get user provided key first!(via ioctl)\n");
		return -EINVAL;
	}

	ret = init_crypto_helper();
	if (ret) {
		pr_err("Failed to initialize basic crypto helpers. (%d)\n",
			ret);
		return ret;
	}
	ret = init_iv_generator(get_key_ptr(),
				HIBERNATE_KEY_BYTES);
	if (ret) {
		pr_err("Failed to init the iv generator. (%d)\n", ret);
		goto out_helper;
	}

	pr_info("Key generated, waiting for data encryption/decrytion.\n");
	return 0;

 out_helper:
	exit_crypto_helper();
	return ret;
}

/* key/salt probing via ioctl. */
dev_t crypto_dev;
static struct class *crypto_dev_class;
static struct cdev crypto_cdev;

#define HIBERNATE_SALT_READ		_IOW('C', 3, struct hibernation_crypto_keys)
#define HIBERNATE_KEY_WRITE		_IOW('C', 4, struct hibernation_crypto_keys)

static DEFINE_MUTEX(crypto_mutex);

static long crypto_ioctl(struct file *file, unsigned int cmd,
			 unsigned long arg)
{
	int ret;

	mutex_lock(&crypto_mutex);
	switch (cmd) {
	case HIBERNATE_SALT_READ:
		if (copy_to_user((void __user *)arg,
				 get_salt_ptr(),
				 HIBERNATE_SALT_BYTES))
			ret = -EFAULT;
		break;
	case HIBERNATE_KEY_WRITE:
		if (copy_from_user(get_key_ptr(),
				   (void __user *)arg,
				   HIBERNATE_KEY_BYTES))
			ret = -EFAULT;
		hib_crypto.keys.user_key_valid = true;
		break;
	default:
		break;
	}
	mutex_unlock(&crypto_mutex);

	return ret;
}

static int crypto_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int crypto_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations crypto_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= crypto_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= crypto_ioctl,
#endif
	.open		= crypto_open,
	.release	= crypto_release,
	.llseek		= noop_llseek,
};

static inline void prepare_crypto_ioctl(void)
{
	/* generate the random salt */
	get_random_bytes(get_salt_ptr(), HIBERNATE_SALT_BYTES);
	/* install the hibernation hooks */
	set_hibernation_ops(&hib_crypto);
}

static int crypto_hibernate_init(void)
{
	if ((alloc_chrdev_region(&crypto_dev, 0, 1, "crypto")) < 0) {
		pr_err("Cannot allocate major number for crypto hibernate.\n");
		return -ENOMEM;
	}

	cdev_init(&crypto_cdev, &crypto_fops);
	crypto_cdev.owner = THIS_MODULE;
	crypto_cdev.ops = &crypto_fops;

	if ((cdev_add(&crypto_cdev, crypto_dev, 1)) < 0) {
		pr_err("Cannot add the crypto device.\n");
		goto r_chrdev;
	}

	crypto_dev_class = class_create(THIS_MODULE,
					"crypto_class");
	if (crypto_dev_class == NULL) {
		pr_err("Cannot create the crypto_class.\n");
		goto r_cdev;
	}

	if ((device_create(crypto_dev_class, NULL, crypto_dev, NULL,
					"crypto_hibernate")) == NULL){
		pr_err("Cannot create the crypto device node.\n");
		goto r_device;
	}
	prepare_crypto_ioctl();

	return 0;

 r_device:
	class_destroy(crypto_dev_class);
 r_cdev:
	cdev_del(&crypto_cdev);
 r_chrdev:
	unregister_chrdev_region(crypto_dev, 1);
	return -EINVAL;
}

static void crypto_hibernate_exit(void)
{
	set_hibernation_ops(NULL);
	device_destroy(crypto_dev_class, crypto_dev);
	class_destroy(crypto_dev_class);
	cdev_del(&crypto_cdev);
	unregister_chrdev_region(crypto_dev, 1);
}

MODULE_AUTHOR("Chen Yu <yu.c.chen@intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Hibernatin crypto facility");

module_init(crypto_hibernate_init);
module_exit(crypto_hibernate_exit);
