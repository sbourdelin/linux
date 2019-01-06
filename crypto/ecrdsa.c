// SPDX-License-Identifier: GPL-2.0+
/*
 * Elliptic Curve (Russian) Digital Signature Algorithm for Cryptographic API
 *
 * Copyright (c) 2019 Vitaly Chikunov <vt@altlinux.org>
 *
 * References:
 * GOST 34.10-2018, GOST R 34.10-2012, RFC 7091, ISO/IEC 14888-3:2018.
 *
 * Historical references:
 * GOST R 34.10-2001, RFC 4357, ISO/IEC 14888-3:2006/Amd 1:2010.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/module.h>
#include <linux/crypto.h>
#include <crypto/streebog.h>
#include <crypto/internal/akcipher.h>
#include <crypto/akcipher.h>
#include <linux/oid_registry.h>

#include "ecc.h"

#define ECRDSA_MAX_SIG_SIZE (2 * 512 / 8)
#define ECRDSA_MAX_DIGITS (512 / 64)

struct ecrdsa_ctx {
	enum OID algo_oid; /* overall public key oid */
	enum OID curve_oid; /* parameter */
	enum OID digest_oid; /* parameter */
	const struct ecc_curve *curve; /* curve from oid */
	unsigned int digest_len; /* parameter (bytes) */
	const char *digest; /* digest name from oid */
	unsigned int key_len; /* key length (bytes) */
	struct ecc_point pub_key;
	u64 _pubp[2][ECRDSA_MAX_DIGITS];
};

/*
 * EC-RDSA uses its own set of curves.
 *
 * cp256{a,b,c} curves first defined for GOST R 34.10-2001 in RFC 4357 (as
 * 256-bit {A,B,C}-ParamSet), but inherited for GOST R 34.10-2012 and
 * proposed for use in R 50.1.114-2016 and RFC 7836 as the 256-bit curves.
 */
/* OID_gostCPSignA 1.2.643.2.2.35.1 */
static u64 cp256a_g_x[] = {
	0x0000000000000001ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull, };
static u64 cp256a_g_y[] = {
	0x22ACC99C9E9F1E14ull, 0x35294F2DDF23E3B1ull,
	0x27DF505A453F2B76ull, 0x8D91E471E0989CDAull, };
static u64 cp256a_p[] = { /* p = 2^256 - 617 */
	0xFFFFFFFFFFFFFD97ull, 0xFFFFFFFFFFFFFFFFull,
	0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull };
static u64 cp256a_n[] = {
	0x45841B09B761B893ull, 0x6C611070995AD100ull,
	0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull };
static u64 cp256a_a[] = { /* a = p - 3 */
	0xFFFFFFFFFFFFFD94ull, 0xFFFFFFFFFFFFFFFFull,
	0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull };
static u64 cp256a_b[] = {
	0x00000000000000a6ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull };
static struct ecc_curve gost_cp256a = {
	.name = "cp256a",
	.g = {
		.x = cp256a_g_x,
		.y = cp256a_g_y,
		.ndigits = 256 / 64,
	},
	.p = cp256a_p,
	.n = cp256a_n,
	.a = cp256a_a,
	.b = cp256a_b
};

/* OID_gostCPSignB 1.2.643.2.2.35.2 */
static u64 cp256b_g_x[] = {
	0x0000000000000001ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull, };
static u64 cp256b_g_y[] = {
	0x744BF8D717717EFCull, 0xC545C9858D03ECFBull,
	0xB83D1C3EB2C070E5ull, 0x3FA8124359F96680ull, };
static u64 cp256b_p[] = { /* p = 2^255 + 3225 */
	0x0000000000000C99ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x8000000000000000ull, };
static u64 cp256b_n[] = {
	0xE497161BCC8A198Full, 0x5F700CFFF1A624E5ull,
	0x0000000000000001ull, 0x8000000000000000ull, };
static u64 cp256b_a[] = { /* a = p - 3 */
	0x0000000000000C96ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x8000000000000000ull, };
static u64 cp256b_b[] = {
	0x2F49D4CE7E1BBC8Bull, 0xE979259373FF2B18ull,
	0x66A7D3C25C3DF80Aull, 0x3E1AF419A269A5F8ull, };
static struct ecc_curve gost_cp256b = {
	.name = "cp256b",
	.g = {
		.x = cp256b_g_x,
		.y = cp256b_g_y,
		.ndigits = 256 / 64,
	},
	.p = cp256b_p,
	.n = cp256b_n,
	.a = cp256b_a,
	.b = cp256b_b
};

/* OID_gostCPSignC 1.2.643.2.2.35.3 */
static u64 cp256c_g_x[] = {
	0x0000000000000000ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull, };
static u64 cp256c_g_y[] = {
	0x366E550DFDB3BB67ull, 0x4D4DC440D4641A8Full,
	0x3CBF3783CD08C0EEull, 0x41ECE55743711A8Cull, };
static u64 cp256c_p[] = {
	0x7998F7B9022D759Bull, 0xCF846E86789051D3ull,
	0xAB1EC85E6B41C8AAull, 0x9B9F605F5A858107ull,
	/* pre-computed value for Barrett's reduction */
	0xedc283cdd217b5a2ull, 0xbac48fc06398ae59ull,
	0x405384d55f9f3b73ull, 0xa51f176161f1d734ull,
	0x0000000000000001ull, };
static u64 cp256c_n[] = {
	0xF02F3A6598980BB9ull, 0x582CA3511EDDFB74ull,
	0xAB1EC85E6B41C8AAull, 0x9B9F605F5A858107ull, };
static u64 cp256c_a[] = { /* a = p - 3 */
	0x7998F7B9022D7598ull, 0xCF846E86789051D3ull,
	0xAB1EC85E6B41C8AAull, 0x9B9F605F5A858107ull, };
static u64 cp256c_b[] = {
	0x000000000000805aull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull, };
static struct ecc_curve gost_cp256c = {
	.name = "cp256c",
	.g = {
		.x = cp256c_g_x,
		.y = cp256c_g_y,
		.ndigits = 256 / 64,
	},
	.p = cp256c_p,
	.n = cp256c_n,
	.a = cp256c_a,
	.b = cp256c_b
};

/* tc512{a,b} curves first recommended in 2013 and then standardized in
 * R 50.1.114-2016 and RFC 7836 for use with GOST R 34.10-2012 (as TC26
 * 512-bit ParamSet{A,B}).
 */
/* OID_gostTC26Sign512A 1.2.643.7.1.2.1.2.1 */
static u64 tc512a_g_x[] = {
	0x0000000000000003ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull, };
static u64 tc512a_g_y[] = {
	0x89A589CB5215F2A4ull, 0x8028FE5FC235F5B8ull,
	0x3D75E6A50E3A41E9ull, 0xDF1626BE4FD036E9ull,
	0x778064FDCBEFA921ull, 0xCE5E1C93ACF1ABC1ull,
	0xA61B8816E25450E6ull, 0x7503CFE87A836AE3ull, };
static u64 tc512a_p[] = { /* p = 2^512 - 569 */
	0xFFFFFFFFFFFFFDC7ull, 0xFFFFFFFFFFFFFFFFull,
	0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull,
	0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull,
	0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, };
static u64 tc512a_n[] = {
	0xCACDB1411F10B275ull, 0x9B4B38ABFAD2B85Dull,
	0x6FF22B8D4E056060ull, 0x27E69532F48D8911ull,
	0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull,
	0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, };
static u64 tc512a_a[] = { /* a = p - 3 */
	0xFFFFFFFFFFFFFDC4ull, 0xFFFFFFFFFFFFFFFFull,
	0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull,
	0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull,
	0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, };
static u64 tc512a_b[] = {
	0x503190785A71C760ull, 0x862EF9D4EBEE4761ull,
	0x4CB4574010DA90DDull, 0xEE3CB090F30D2761ull,
	0x79BD081CFD0B6265ull, 0x34B82574761CB0E8ull,
	0xC1BD0B2B6667F1DAull, 0xE8C2505DEDFC86DDull, };
static struct ecc_curve gost_tc512a = {
	.name = "tc512a",
	.g = {
		.x = tc512a_g_x,
		.y = tc512a_g_y,
		.ndigits = 512 / 64,
	},
	.p = tc512a_p,
	.n = tc512a_n,
	.a = tc512a_a,
	.b = tc512a_b
};

/* OID_gostTC26Sign512B 1.2.643.7.1.2.1.2.2 */
static u64 tc512b_g_x[] = {
	0x0000000000000002ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull, };
static u64 tc512b_g_y[] = {
	0x7E21340780FE41BDull, 0x28041055F94CEEECull,
	0x152CBCAAF8C03988ull, 0xDCB228FD1EDF4A39ull,
	0xBE6DD9E6C8EC7335ull, 0x3C123B697578C213ull,
	0x2C071E3647A8940Full, 0x1A8F7EDA389B094Cull, };
static u64 tc512b_p[] = { /* p = 2^511 + 111 */
	0x000000000000006Full, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x8000000000000000ull, };
static u64 tc512b_n[] = {
	0xC6346C54374F25BDull, 0x8B996712101BEA0Eull,
	0xACFDB77BD9D40CFAull, 0x49A1EC142565A545ull,
	0x0000000000000001ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x8000000000000000ull, };
static u64 tc512b_a[] = { /* a = p - 3 */
	0x000000000000006Cull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x0000000000000000ull,
	0x0000000000000000ull, 0x8000000000000000ull, };
static u64 tc512b_b[] = {
	0xFB8CCBC7C5140116ull, 0x50F78BEE1FA3106Eull,
	0x7F8B276FAD1AB69Cull, 0x3E965D2DB1416D21ull,
	0xBF85DC806C4B289Full, 0xB97C7D614AF138BCull,
	0x7E3E06CF6F5E2517ull, 0x687D1B459DC84145ull, };
static struct ecc_curve gost_tc512b = {
	.name = "tc512b",
	.g = {
		.x = tc512b_g_x,
		.y = tc512b_g_y,
		.ndigits = 512 / 64,
	},
	.p = tc512b_p,
	.n = tc512b_n,
	.a = tc512b_a,
	.b = tc512b_b
};

static const struct ecc_curve *get_curve_by_oid(enum OID oid)
{
	switch (oid) {
	case OID_gostCPSignA:
	case OID_gostTC26Sign256B:
		return &gost_cp256a;
	case OID_gostCPSignB:
	case OID_gostTC26Sign256C:
		return &gost_cp256b;
	case OID_gostCPSignC:
	case OID_gostTC26Sign256D:
		return &gost_cp256c;
	case OID_gostTC26Sign512A:
		return &gost_tc512a;
	case OID_gostTC26Sign512B:
		return &gost_tc512b;
	default:
		return NULL;
	}
}

static int ecrdsa_sign(struct akcipher_request *req)
{
	return -ENOSYS;
}

static int ecrdsa_verify2(struct akcipher_request *req)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	struct ecrdsa_ctx *ctx = akcipher_tfm_ctx(tfm);
	unsigned char sig[ECRDSA_MAX_SIG_SIZE + 1];
	unsigned int ndigits = req->digest_len / sizeof(u64);
	u64 e[ECRDSA_MAX_DIGITS]; /* h \mod q */
	u64 r[ECRDSA_MAX_DIGITS]; /* witness (r) */
	u64 s[ECRDSA_MAX_DIGITS]; /* second part of sig (s) */
	u64 v[ECRDSA_MAX_DIGITS]; /* e^{-1} \mod q */
	u64 z1[ECRDSA_MAX_DIGITS];
	u64 z2[ECRDSA_MAX_DIGITS];
	u64 cpt[2][ECRDSA_MAX_DIGITS];
	struct ecc_point cc = ECC_POINT_INIT(cpt[0], cpt[1], ndigits);

	/*
	 * Digest value, digest algorithm, and curve (modulus) should have the same
	 * length (256 or 512 bits), public key and signature should be twice bigger
	 * (plus 1 byte for BIT STRING of signature metadata).
	 */
	if (!ctx->curve ||
	    !ctx->digest ||
	    !req->digest ||
	    !ctx->pub_key.x ||
	    req->digest_len != ctx->digest_len ||
	    req->digest_len != ctx->curve->g.ndigits * sizeof(u64) ||
	    ctx->pub_key.ndigits != ctx->curve->g.ndigits ||
	    req->digest_len * 2 != req->src_len - 1 ||
	    WARN_ON(req->src_len > sizeof(sig)))
		return -EBADMSG;

	sg_copy_to_buffer(req->src, sg_nents_for_len(req->src, req->src_len),
			  sig, req->src_len);

	if (sig[0]) /* invalid BIT STRING */
		return -EBADMSG;

	vli_from_be64(s, sig + 1, ndigits);
	vli_from_be64(r, sig + 1 + ndigits * sizeof(u64), ndigits);

	/* Step 1: verify that 0 < r < q, 0 < s < q */
	if (vli_is_zero(r, ndigits) ||
	    vli_cmp(r, ctx->curve->n, ndigits) == 1 ||
	    vli_is_zero(s, ndigits) ||
	    vli_cmp(s, ctx->curve->n, ndigits) == 1)
		return -EKEYREJECTED;

	/* Step 2: calculate hash (h) of the message (passed as input) */
	/* Step 3: calculate e = h \mod q */
	vli_from_le64(e, req->digest, ndigits);
	if (vli_cmp(e, ctx->curve->n, ndigits) == 1)
		vli_sub(e, e, ctx->curve->n, ndigits);
	if (vli_is_zero(e, ndigits))
		e[0] = 1;

	/* Step 4: calculate v = e^{-1} \mod q */
	vli_mod_inv(v, e, ctx->curve->n, ndigits);

	/* Step 5: calculate z_1 = sv \mod q, z_2 = -rv \mod q */
	vli_mod_mult_slow(z1, s, v, ctx->curve->n, ndigits);
	{
		u64 _r[ECRDSA_MAX_DIGITS];

		vli_sub(_r, ctx->curve->n, r, ndigits);
		vli_mod_mult_slow(z2, _r, v, ctx->curve->n, ndigits);
	}

	/* Step 6: calculate point C = z_1P + z_2Q, and R = x_c \mod q */
	ecc_point_mult_shamir(&cc, z1, &ctx->curve->g, z2, &ctx->pub_key,
			      ctx->curve);
	if (vli_cmp(cc.x, ctx->curve->n, ndigits) == 1)
		vli_sub(cc.x, cc.x, ctx->curve->n, ndigits);

	/* Step 7: if R == r signature is valid */
	if (!vli_cmp(cc.x, r, ndigits))
		return 0;
	else
		return -EKEYREJECTED;
}

/* Parse DER encoded subjectPublicKey. */
static int ecrdsa_set_pub_key(struct crypto_akcipher *tfm, const void *ber,
			      unsigned int len)
{
	struct ecrdsa_ctx *ctx = akcipher_tfm_ctx(tfm);
	unsigned int ndigits;
	const u8 *k = ber;
	unsigned int offset;

	/* First chance to zero ctx */
	memset(ctx, 0, sizeof(*ctx));

	if (len < 3 ||
	    k[0] != 0x04 || /* OCTET STRING */
	    (k[1] < 0x80 && len != k[1] + 2) ||
	    (k[1] == 0x81 && len != k[2] + 3) ||
	    k[1] > 0x81)
		return -EBADMSG;
	offset = (k[1] < 0x80)? 2 : 3;
	k += offset;
	len -= offset;
	/* Key is two 256- or 512-bit coordinates. */
	if (len != (2 * 256 / 8) &&
	    len != (2 * 512 / 8))
		return -ENOPKG;
	ndigits = len / sizeof(u64) / 2;
	ctx->pub_key = ECC_POINT_INIT(ctx->_pubp[0], ctx->_pubp[1], ndigits);
	vli_from_le64(ctx->pub_key.x, k, ndigits);
	vli_from_le64(ctx->pub_key.y, k + ndigits * sizeof(u64), ndigits);

	return 0;
}

/* Parse DER encoded SubjectPublicKeyInfo.AlgorithmIdentifier.parameters. */
static int ecrdsa_set_params(struct crypto_akcipher *tfm, enum OID algo,
			     const void *params, unsigned int paramlen)
{
	struct ecrdsa_ctx *ctx = akcipher_tfm_ctx(tfm);
	const u8 *p = params;
	int i;

	if (algo == OID_gost2012PublicKey256) {
		ctx->digest	= "streebog256";
		ctx->digest_oid	= OID_gost2012Digest256;
		ctx->digest_len	= 256 / 8;
	} else if (algo == OID_gost2012PublicKey512) {
		ctx->digest	= "streebog512";
		ctx->digest_oid	= OID_gost2012Digest512;
		ctx->digest_len	= 512 / 8;
	} else
		return -ENOPKG;
	ctx->curve = NULL;
	ctx->curve_oid = 0;
	ctx->algo_oid = algo;

	for (i = 0; i < paramlen; i += p[i + 1] + 2) {
		const struct ecc_curve *curve;
		enum OID oid;

		if (paramlen - i < 2 ||
		    p[i] != 0x06 || /* OBJECT IDENTIFIER */
		    p[i + 1] > paramlen - i - 2)
			return -EBADMSG;
		oid = look_up_OID(p + i + 2, p[i + 1]);
		if (oid == OID__NR)
			return -ENOPKG;

		if (oid == OID_gost2012Digest256 ||
		    oid == OID_gost2012Digest512) {
			if (oid != ctx->digest_oid)
				return -ENOPKG;
		} else {
			curve = get_curve_by_oid(oid);
			if (!curve || ctx->curve)
				return -ENOPKG;
			ctx->curve = curve;
			ctx->curve_oid = oid;
		}
	}
	/* Sizes of algo, curve, pub_key, and digest should match each other. */
	if (!ctx->curve ||
	    ctx->curve->g.ndigits * sizeof(u64) != ctx->digest_len ||
	    ctx->curve->g.ndigits != ctx->pub_key.ndigits)
		return -ENOPKG;

	/* First chance to validate the public key. */
	if (ecc_is_pubkey_valid_partial(ctx->curve, &ctx->pub_key))
		return -EKEYREJECTED;

	return 0;
}

static int ecrdsa_set_priv_key(struct crypto_akcipher *tfm, const void *key,
			       unsigned int keylen)
{
	return -ENOSYS;
}

static unsigned int ecrdsa_max_size(struct crypto_akcipher *tfm)
{
	struct ecrdsa_ctx *ctx = akcipher_tfm_ctx(tfm);

	/* verify2 doesn't need any output, so it's just informational
	 * for keyctl for a key size.
	 */
	return ctx->pub_key.ndigits * sizeof(u64);
}

static void ecrdsa_exit_tfm(struct crypto_akcipher *tfm)
{
}

static struct akcipher_alg ecrdsa_alg = {
	.sign		= ecrdsa_sign,
	.verify2	= ecrdsa_verify2,
	.set_priv_key	= ecrdsa_set_priv_key,
	.set_pub_key	= ecrdsa_set_pub_key,
	.set_params	= ecrdsa_set_params,
	.max_size	= ecrdsa_max_size,
	.exit		= ecrdsa_exit_tfm,
	.reqsize	= sizeof(struct ecrdsa_ctx),
	.base = {
		.cra_name	 = "ecrdsa",
		.cra_driver_name = "ecrdsa-generic",
		.cra_priority	 = 100,
		.cra_module	 = THIS_MODULE,
		.cra_ctxsize	 = sizeof(struct ecrdsa_ctx),
	},
};

static int __init ecrdsa_mod_init(void)
{
	return crypto_register_akcipher(&ecrdsa_alg);
}

static void __exit ecrdsa_mod_fini(void)
{
	crypto_unregister_akcipher(&ecrdsa_alg);
}

module_init(ecrdsa_mod_init);
module_exit(ecrdsa_mod_fini);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vitaly Chikunov <vt@altlinux.org>");
MODULE_DESCRIPTION("EC-RDSA generic algorithm");
MODULE_ALIAS_CRYPTO("ecrdsa");
