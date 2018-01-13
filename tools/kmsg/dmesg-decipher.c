// SPDX-License-Identifier: GPL-2.0
/*
 * dmesg-decipher.c
 *
 * A sample utility to decrypt an encrypted dmesg output, for
 * development with kernels having kmsg encryption enabled.
 *
 * base64 decoding code taken from lib/base64-armor.c
 *
 * Copyright (c) Dan Aloni, 2017
 *
 * Compile with:
 *
 *     gcc -O2 -Wall $(pkg-config --libs openssl) \
 *           dmesg-decipher -o dmesg-decipher
 */

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs7.h>

#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * The following is based on code from:
 *
 *    https://wiki.openssl.org/index.php/EVP_Authenticated_Encryption_and_Decryption
 */
static int aes_256_gcm_decrypt(unsigned char *ciphertext, size_t ciphertext_len,
			       unsigned char *aad, size_t aad_len,
			       unsigned char *tag, unsigned char *key,
			       unsigned char *iv, size_t iv_len,
			       unsigned char *plaintext)
{
	EVP_CIPHER_CTX *ctx;
	int len;
	int plaintext_len;
	int ret = -1;

	/* Create and initialise the context */
	ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return -1;

	/* Initialise the decryption operation. */
	if (!EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL))
		goto free;

	/* Set IV length. Not necessary if this is 12 bytes (96 bits) */
	if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, NULL))
		goto free;

	/* Initialise key and IV */
	if (!EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv))
		goto free;

	/* Provide any AAD data. This can be called zero or more times as
	 * required
	 */
	if (aad_len != 0)
		if (!EVP_DecryptUpdate(ctx, NULL, &len, aad, aad_len))
			goto free;

	/* Provide the message to be decrypted, and obtain the plaintext output.
	 * EVP_DecryptUpdate can be called multiple times if necessary
	 */
	if (!EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext,
			       ciphertext_len))
		goto free;
	plaintext_len = len;

	/* Set expected tag value. Works in OpenSSL 1.0.1d and later */
	if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag))
		goto free;

	/* Finalise the decryption. A positive return value indicates success,
	 * anything else is a failure - the plaintext is not trustworthy.
	 */
	ret = EVP_DecryptFinal_ex(ctx, plaintext + len, &len);

free:
	/* Clean up */
	EVP_CIPHER_CTX_free(ctx);

	if (ret > 0) {
		/* Success */
		plaintext_len += len;
		return plaintext_len;
	}

	/* Verify failed */
	return -1;
}

static int decode_bits(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	if (c == '=')
		return 0; /* just non-negative, please */
	return -EINVAL;
}

static int base64_unarmor(char *dst, int dst_max, const char *src,
			  const char *end)
{
	int olen = 0;

	while (src < end) {
		int a, b, c, d;

		if (src[0] == '\n') {
			src++;
			continue;
		}
		if (src + 4 > end)
			return -EINVAL;
		a = decode_bits(src[0]);
		b = decode_bits(src[1]);
		c = decode_bits(src[2]);
		d = decode_bits(src[3]);
		if (a < 0 || b < 0 || c < 0 || d < 0)
			return -EINVAL;

		if (dst_max < 1)
			return -ENOSPC;
		*dst++ = (a << 2) | (b >> 4);
		dst_max--;
		if (src[2] == '=')
			return olen + 1;
		if (dst_max < 1)
			return -ENOSPC;
		*dst++ = ((b & 15) << 4) | (c >> 2);
		dst_max--;
		if (src[3] == '=')
			return olen + 2;
		if (dst_max < 1)
			return -ENOSPC;
		*dst++ = ((c & 3) << 6) | d;
		dst_max--;
		olen += 3;
		src += 4;
	}
	return olen;
}

static int parse_int_regex_match(const char *source, regmatch_t match,
				 size_t *output)
{
	char decimal_number[0x10] = {
		0,
	};
	size_t len = match.rm_eo - match.rm_so;

	if (len >= sizeof(decimal_number))
		return -1;

	memcpy(&decimal_number[0], &source[match.rm_so], len);

	*output = atoi(decimal_number);
	return 0;
}

static const char session_key_pattern[] = "(.*)K:([0-9a-zA-Z~+/=]+)";
static const char message_pattern[] =
	"(.*)M:([0-9a-zA-Z~+/=]+),([0-9]+),([0-9]+)";

static int decrypt_message(char *line, regmatch_t *matches, uint8_t *sess_key)
{
	char plain_text[0x1000], *enc;
	uint8_t cipher_msg_bin[0x1000];
	size_t cipher_msg_size = sizeof(cipher_msg_bin);
	size_t cipher_size;
	const regmatch_t prefix = matches[1];
	const regmatch_t ciphermsg = matches[2];
	const regmatch_t auth_str_len = matches[3];
	const regmatch_t iv_str_len = matches[4];
	size_t auth_len;
	size_t iv_len;
	int ret;

	ret = parse_int_regex_match(line, auth_str_len, &auth_len);
	if (ret)
		return -1;

	ret = parse_int_regex_match(line, iv_str_len, &iv_len);
	if (ret)
		return -1;

	for (enc = &line[ciphermsg.rm_so]; enc < &line[ciphermsg.rm_eo]; enc++)
		if (*enc == '~')
			*enc = '\n';

	ret = base64_unarmor((char *)cipher_msg_bin, cipher_msg_size,
			     &line[ciphermsg.rm_so], &line[ciphermsg.rm_eo]);
	if (ret < 0) {
		fprintf(stderr, "error decoding base64 message (code = %d)\n",
			ret);
		return -1;
	}

	cipher_msg_size = ret;

	if (iv_len >= cipher_msg_size || auth_len >= cipher_msg_size
	    || auth_len + iv_len > cipher_msg_size) {
		return -1;
	}

	cipher_size = cipher_msg_size - auth_len - iv_len;

	ret = aes_256_gcm_decrypt(/* Ciphertext */
				  (uint8_t *)cipher_msg_bin, cipher_size,

				  /* AAD */
				  NULL, 0,

				  /* tag */
				  (uint8_t *)&cipher_msg_bin[cipher_size],

				  /* key */
				  sess_key,

				  /* IV */
				  (uint8_t *)&cipher_msg_bin[cipher_size
							     + auth_len],
				  iv_len,

				  /* Plain text */
				  (uint8_t *)plain_text);
	if (ret > 0) {
		fwrite(line, prefix.rm_eo, 1, stdout);
		fwrite(plain_text, ret, 1, stdout);
		fwrite("\n", 1, 1, stdout);
	}

	return ret;
}

int main(int argc, char **argv)
{
	BIO *tbio = NULL;
	RSA *rsa;
	int ret = 1;
	char line[0x1000];
	uint8_t enc_sess_key[0x200];
	uint8_t sess_key[0x200] = {
		0,
	};
	bool got_key = false;

	OpenSSL_add_all_algorithms();
	ERR_load_crypto_strings();

	regex_t session_key_regex;
	regex_t message_regex;

	ret = regcomp(&session_key_regex, session_key_pattern, REG_EXTENDED);
	if (ret)
		goto err;

	ret = regcomp(&message_regex, message_pattern, REG_EXTENDED);
	if (ret)
		goto err;

	if (argc < 2) {
		fprintf(stderr, "not enough parameters\n");
		return -1;
	}

	/* Read in recipient certificate and private key */
	tbio = BIO_new_file(argv[1], "r");
	if (!tbio) {
		fprintf(stderr, "BIO_new_file - error\n");
		goto err;
	}

	rsa = PEM_read_bio_RSAPrivateKey(tbio, NULL, NULL, NULL);
	if (!rsa)
		goto err;

	while (true) {
		regmatch_t matches[5];

		if (!fgets(line, sizeof(line), stdin))
			break;

		if (!got_key
		    && !regexec(&session_key_regex, line, 5, matches, 0)) {
			const regmatch_t match = matches[2];
			size_t enc_sess_key_size = sizeof(enc_sess_key);
			char *enc;

			for (enc = &line[match.rm_so]; enc < &line[match.rm_eo];
			     enc++)
				if (*enc == '~')
					*enc = '\n';

			ret = base64_unarmor(
				(char *)&enc_sess_key, enc_sess_key_size,
				&line[match.rm_so], &line[match.rm_eo]);
			if (ret < 0) {
				fprintf(stderr,
					"error decoding session key"
					" (code = %d)\n",
					ret);
				return -1;
			}

			enc_sess_key_size = ret;

			ret = RSA_private_decrypt(enc_sess_key_size,
						  enc_sess_key, sess_key, rsa,
						  RSA_PKCS1_PADDING);
			if (ret < 0)
				goto err;

			got_key = true;
		}

		if (!regexec(&message_regex, line, 5, matches, 0)) {
			if (!got_key) {
				fprintf(stderr,
					"session key must precede messages\n");
				break;
			}

			ret = decrypt_message(line, matches, sess_key);
			if (ret < 0) {
				fprintf(stderr,
					"error decrypting message"
					" (code = %d)\n",
					ret);
				break;
			}
		}
	}

	regfree(&session_key_regex);
	regfree(&message_regex);

err:
	return -1;
}
