/*
 * IMA support for appraising module-style appended signatures.
 *
 * Copyright (C) 2017  IBM Corporation
 *
 * Author:
 * Thiago Jung Bauermann <bauerman@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation (version 2 of the License).
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/types.h>
#include <linux/module_signature.h>
#include <keys/asymmetric-type.h>
#include <crypto/pkcs7.h>

#include "ima.h"

struct modsig_hdr {
	uint8_t type;		/* Should be IMA_MODSIG. */
	const void *data;	/* Pointer to data covered by pkcs7_msg. */
	size_t data_len;
	struct pkcs7_message *pkcs7_msg;
	int raw_pkcs7_len;

	/* This will be in the measurement list if required by the template. */
	struct evm_ima_xattr_data raw_pkcs7;
};

/**
 * ima_hook_supports_modsig - can the policy allow modsig for this hook?
 *
 * modsig is only supported by hooks using ima_post_read_file, because only they
 * preload the contents of the file in a buffer. FILE_CHECK does that in some
 * cases, but not when reached from vfs_open. POLICY_CHECK can support it, but
 * it's not useful in practice because it's a text file so deny.
 */
bool ima_hook_supports_modsig(enum ima_hooks func)
{
	switch (func) {
	case FIRMWARE_CHECK:
	case KEXEC_KERNEL_CHECK:
	case KEXEC_INITRAMFS_CHECK:
		return true;
	default:
		return false;
	}
}

int ima_read_modsig(const void *buf, loff_t buf_len,
		    struct evm_ima_xattr_data **xattr_value,
		    int *xattr_len)
{
	const size_t marker_len = sizeof(MODULE_SIG_STRING) - 1;
	const struct module_signature *sig;
	struct modsig_hdr *hdr;
	size_t sig_len;
	const void *p;
	int rc;

	if (buf_len <= marker_len + sizeof(*sig))
		return -ENOENT;

	p = buf + buf_len - marker_len;
	if (memcmp(p, MODULE_SIG_STRING, marker_len))
		return -ENOENT;

	buf_len -= marker_len;
	sig = (const struct module_signature *) (p - sizeof(*sig));

	rc = validate_module_sig(sig, buf_len);
	if (rc)
		return rc;

	sig_len = be32_to_cpu(sig->sig_len);
	buf_len -= sig_len + sizeof(*sig);

	hdr = kmalloc(sizeof(*hdr) + sig_len, GFP_KERNEL);
	if (!hdr)
		return -ENOMEM;

	hdr->pkcs7_msg = pkcs7_parse_message(buf + buf_len, sig_len);
	if (IS_ERR(hdr->pkcs7_msg)) {
		rc = PTR_ERR(hdr->pkcs7_msg);
		kfree(hdr);
		return rc;
	}

	memcpy(hdr->raw_pkcs7.data, buf + buf_len, sig_len);
	hdr->raw_pkcs7_len = sig_len + 1;
	hdr->raw_pkcs7.type = IMA_MODSIG;

	hdr->type = IMA_MODSIG;
	hdr->data = buf;
	hdr->data_len = buf_len;

	*xattr_value = (typeof(*xattr_value)) hdr;
	*xattr_len = sizeof(*hdr);

	return 0;
}

int ima_modsig_serialize_data(struct evm_ima_xattr_data *hdr,
			      struct evm_ima_xattr_data **data, int *data_len)
{
	struct modsig_hdr *modsig = (struct modsig_hdr *) hdr;

	*data = &modsig->raw_pkcs7;
	*data_len = modsig->raw_pkcs7_len;

	return 0;
}

int ima_modsig_verify(const unsigned int keyring_id,
		      struct evm_ima_xattr_data *hdr)
{
	struct modsig_hdr *modsig = (struct modsig_hdr *) hdr;
	struct key *trusted_keys = integrity_keyring_from_id(keyring_id);

	if (IS_ERR(trusted_keys))
		return -EINVAL;

	return verify_pkcs7_message_sig(modsig->data, modsig->data_len,
					modsig->pkcs7_msg, trusted_keys,
					VERIFYING_MODULE_SIGNATURE, NULL, NULL);
}

void ima_free_xattr_data(struct evm_ima_xattr_data *hdr)
{
	if (!hdr)
		return;

	if (hdr->type == IMA_MODSIG) {
		struct modsig_hdr *modsig = (struct modsig_hdr *) hdr;

		pkcs7_free_message(modsig->pkcs7_msg);
	}

	kfree(hdr);
}
