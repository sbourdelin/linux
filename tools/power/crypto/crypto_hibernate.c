// SPDX-License-Identifier: GPL-2.0
/*
 * The hibernation key based derived function algorithm
 *
 * Copyright (C) 2004 Sam Hocevar <sam@hocevar.net>
 * Copyright (C) 2018 Theodore Ts'o <tytso@mit.edu>
 * (copied from e2fsprogs)
 *
 * The key derivation is based on a simplified implementation
 * of PBKDF2 in e2fsprogs - both the key length and the hash
 * bytes are the same - 512bits. crypto_hibernate will firstly
 * probe the user for passphrase and salt, then uses them to
 * generate a 512bits AES key by SHA512 hash and PBKDF2.
 *
 * Usage:
 * 1. install the kernel module:
 *    modprobe crypto_hibernation
 * 2. run this tool to generate the key from
 *    user provided passphrase (salt is read from kernel)
 * 3. launch the hibernation process, the kernel
 *    uses the key from step 2 to encrypt the
 *    hibernation snapshot
 * 4. resume the system and the initrd will
 *    launch cryto_hibernate to read previous salt
 *    from kernel and probe the user passphrase
 *    and generate the same key
 * 5. kernel uses this key to decrypt the hibernation
 *    snapshot.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Library
 * General Public License, version 2.
 * %End-Header%
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#define PBKDF2_ITERATIONS          0xFFFF
#define SHA512_BLOCKSIZE 128
#define SHA512_LENGTH 64
#define SALT_BYTES	16
#define SYM_KEY_BYTES SHA512_LENGTH
#define TOTAL_USER_INFO_LEN	(SALT_BYTES+SYM_KEY_BYTES)
#define MAX_PASSPHRASE_SIZE	1024

struct hibernation_crypto_keys {
	char derived_key[SYM_KEY_BYTES];
	char salt[SALT_BYTES];
	bool valid;
};

struct hibernation_crypto_keys hib_keys;

static char *get_key_ptr(void)
{
	return hib_keys.derived_key;
}

static char *get_salt_ptr(void)
{
	return hib_keys.salt;
}

static const unsigned int K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL,
    0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL,
    0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL,
    0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL, 0x983e5152UL,
    0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL,
    0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL,
    0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL,
    0xd6990624UL, 0xf40e3585UL, 0x106aa070UL, 0x19a4c116UL, 0x1e376c08UL,
    0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL,
    0x682e6ff3UL, 0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

/* Various logical functions */
#define Ch(x,y,z)       (z ^ (x & (y ^ z)))
#define Maj(x,y,z)      (((x | y) & z) | (x & y))
#define S(x, n)         RORc((x),(n))
#define R(x, n)         (((x)&0xFFFFFFFFUL)>>(n))
#define Sigma0(x)       (S(x, 2) ^ S(x, 13) ^ S(x, 22))
#define Sigma1(x)       (S(x, 6) ^ S(x, 11) ^ S(x, 25))
#define Gamma0(x)       (S(x, 7) ^ S(x, 18) ^ R(x, 3))
#define Gamma1(x)       (S(x, 17) ^ S(x, 19) ^ R(x, 10))
#define RORc(x, y) ( ((((unsigned int)(x)&0xFFFFFFFFUL)>>(unsigned int)((y)&31)) | ((unsigned int)(x)<<(unsigned int)(32-((y)&31)))) & 0xFFFFFFFFUL)

#define RND(a,b,c,d,e,f,g,h,i)                         \
     t0 = h + Sigma1(e) + Ch(e, f, g) + K[i] + W[i];   \
     t1 = Sigma0(a) + Maj(a, b, c);                    \
     d += t0;                                          \
     h  = t0 + t1;

#define STORE64H(x, y) \
	do { \
		(y)[0] = (unsigned char)(((x)>>56)&255);\
		(y)[1] = (unsigned char)(((x)>>48)&255);\
		(y)[2] = (unsigned char)(((x)>>40)&255);\
		(y)[3] = (unsigned char)(((x)>>32)&255);\
		(y)[4] = (unsigned char)(((x)>>24)&255);\
		(y)[5] = (unsigned char)(((x)>>16)&255);\
		(y)[6] = (unsigned char)(((x)>>8)&255);\
		(y)[7] = (unsigned char)((x)&255); } while(0)

#define STORE32H(x, y)                                                                     \
  do { (y)[0] = (unsigned char)(((x)>>24)&255); (y)[1] = (unsigned char)(((x)>>16)&255);   \
       (y)[2] = (unsigned char)(((x)>>8)&255); (y)[3] = (unsigned char)((x)&255); } while(0)

#define LOAD32H(x, y)                            \
  do { x = ((unsigned int)((y)[0] & 255)<<24) | \
           ((unsigned int)((y)[1] & 255)<<16) | \
           ((unsigned int)((y)[2] & 255)<<8)  | \
           ((unsigned int)((y)[3] & 255)); } while(0)

struct sha512_state {
	unsigned long long length;
	unsigned int state[8], curlen;
	unsigned char buf[64];
};

/* This is a highly simplified version from libtomcrypt */
struct hash_state {
	struct sha512_state sha512;
};

static void sha512_compress(struct hash_state * md, const unsigned char *buf)
{
	unsigned int S[8], W[64], t0, t1;
	unsigned int t;
	int i;

	/* copy state into S */
	for (i = 0; i < 8; i++) {
		S[i] = md->sha512.state[i];
	}

	/* copy the state into 512-bits into W[0..15] */
	for (i = 0; i < 16; i++) {
		LOAD32H(W[i], buf + (4*i));
	}

	/* fill W[16..63] */
	for (i = 16; i < 64; i++) {
		W[i] = Gamma1(W[i - 2]) + W[i - 7] + Gamma0(W[i - 15]) + W[i - 16];
	}

	/* Compress */
	for (i = 0; i < 64; ++i) {
		RND(S[0],S[1],S[2],S[3],S[4],S[5],S[6],S[7],i);
		t = S[7]; S[7] = S[6]; S[6] = S[5]; S[5] = S[4];
		S[4] = S[3]; S[3] = S[2]; S[2] = S[1]; S[1] = S[0]; S[0] = t;
	}

	/* feedback */
	for (i = 0; i < 8; i++) {
		md->sha512.state[i] = md->sha512.state[i] + S[i];
	}
}

static void sha512_init(struct hash_state * md)
{
	md->sha512.curlen = 0;
	md->sha512.length = 0;
	md->sha512.state[0] = 0x6A09E667UL;
	md->sha512.state[1] = 0xBB67AE85UL;
	md->sha512.state[2] = 0x3C6EF372UL;
	md->sha512.state[3] = 0xA54FF53AUL;
	md->sha512.state[4] = 0x510E527FUL;
	md->sha512.state[5] = 0x9B05688CUL;
	md->sha512.state[6] = 0x1F83D9ABUL;
	md->sha512.state[7] = 0x5BE0CD19UL;
}

#define MIN(x, y) ( ((x)<(y))?(x):(y) )

static void sha512_process(struct hash_state * md, const unsigned char *in, unsigned long inlen)
{
	unsigned long n;

	while (inlen > 0) {
		if (md->sha512.curlen == 0 && inlen >= SHA512_BLOCKSIZE) {
			sha512_compress(md, in);
			md->sha512.length += SHA512_BLOCKSIZE * 8;
			in += SHA512_BLOCKSIZE;
			inlen -= SHA512_BLOCKSIZE;
		} else {
			n = MIN(inlen, (SHA512_BLOCKSIZE - md->sha512.curlen));
			memcpy(md->sha512.buf + md->sha512.curlen, in, (size_t)n);
			md->sha512.curlen += n;
			in += n;
			inlen -= n;
			if (md->sha512.curlen == SHA512_BLOCKSIZE) {
				sha512_compress(md, md->sha512.buf);
				md->sha512.length += 8*SHA512_BLOCKSIZE;
				md->sha512.curlen = 0;
			}
		}
	}
}

static void sha512_done(struct hash_state * md, unsigned char *out)
{
	int i;

	/* increase the length of the message */
	md->sha512.length += md->sha512.curlen * 8;

	/* append the '1' bit */
	md->sha512.buf[md->sha512.curlen++] = (unsigned char)0x80;

	/* if the length is currently above 56 bytes we append zeros
	 * then compress.  Then we can fall back to padding zeros and length
	 * encoding like normal.
	 */
	if (md->sha512.curlen > 56) {
	while (md->sha512.curlen < 64) {
		md->sha512.buf[md->sha512.curlen++] = (unsigned char)0;
	}
	sha512_compress(md, md->sha512.buf);
	md->sha512.curlen = 0;
	}

	/* pad upto 56 bytes of zeroes */
	while (md->sha512.curlen < 56) {
		md->sha512.buf[md->sha512.curlen++] = (unsigned char)0;
	}

	/* store length */
	STORE64H(md->sha512.length, md->sha512.buf+56);
	sha512_compress(md, md->sha512.buf);

	/* copy output */
	for (i = 0; i < 8; i++) {
		STORE32H(md->sha512.state[i], out+(4*i));
	}
}

void start_sha512(const unsigned char *in, unsigned long in_size,
		   unsigned char out[SHA512_LENGTH])
{
	struct hash_state md;

	sha512_init(&md);
	sha512_process(&md, in, in_size);
	sha512_done(&md, out);
}

static void pbkdf2_sha512(const char *passphrase, const char *salt,
			  unsigned int count,
			  char *derived_key)
{
	size_t passphrase_size = strlen(passphrase);
	unsigned char buf[SHA512_LENGTH + MAX_PASSPHRASE_SIZE] = {0};
	unsigned char tempbuf[SHA512_LENGTH] = {0};
	char final[SHA512_LENGTH] = {0};
	unsigned char saltbuf[SALT_BYTES + MAX_PASSPHRASE_SIZE] = {0};
	int actual_buf_len = SHA512_LENGTH + passphrase_size;
	int actual_saltbuf_len = SALT_BYTES + passphrase_size;
	unsigned int x, y;
	unsigned int *final_u32 = (unsigned int *)final;
	unsigned int *temp_u32 = (unsigned int *)tempbuf;

	memcpy(saltbuf, salt, SALT_BYTES);
	memcpy(&saltbuf[SALT_BYTES], passphrase, passphrase_size);
	memcpy(&buf[SHA512_LENGTH], passphrase, passphrase_size);

	for (x = 0; x < count; ++x) {
		if (x == 0) {
			start_sha512(saltbuf, actual_saltbuf_len, tempbuf);
		} else {
			/*
			 * buf: [previous hash || passphrase]
			 */
			memcpy(buf, tempbuf, SHA512_LENGTH);
			start_sha512(buf, actual_buf_len, tempbuf);
		}
		for (y = 0; y < (sizeof(final) / sizeof(*final_u32)); ++y)
			final_u32[y] = final_u32[y] ^ temp_u32[y];
	}
	memcpy(derived_key, final, SYM_KEY_BYTES);
}

#define HIBERNATE_SALT_READ      _IOW('C', 3, struct hibernation_crypto_keys)
#define HIBERNATE_KEY_WRITE     _IOW('C', 4, struct hibernation_crypto_keys)

static int disable_echo(struct termios *saved_settings)
{
	struct termios current_settings;
	int rc = 0;

	rc = tcgetattr(0, &current_settings);
	if (rc)
		return rc;
	*saved_settings = current_settings;
	current_settings.c_lflag &= ~ECHO;
	rc = tcsetattr(0, TCSANOW, &current_settings);

	return rc;
}

static void get_passphrase(char *passphrase, int len)
{
	char *p;
	struct termios current_settings;

	assert(len > 0);
	disable_echo(&current_settings);
	p = fgets(passphrase, len, stdin);
	tcsetattr(0, TCSANOW, &current_settings);
	printf("\n");
	if (!p) {
		printf("Aborting.\n");
		exit(1);
	}
	p = strrchr(passphrase, '\n');
	if (!p)
		p = passphrase + len - 1;
	*p = '\0';
}

#define CRYPTO_FILE	"/dev/crypto_hibernate"

static int write_keys(void)
{
	int fd;

	fd = open(CRYPTO_FILE, O_RDWR);
	if (fd < 0) {
		printf("Cannot open device file...\n");
		return -EINVAL;
	}
	ioctl(fd, HIBERNATE_KEY_WRITE, get_key_ptr());
	return 0;
}

static int read_salt(void)
{
	int fd;

	fd = open(CRYPTO_FILE, O_RDWR);
	if (fd < 0) {
		printf("Cannot open device file...\n");
		return -EINVAL;
	}
	ioctl(fd, HIBERNATE_SALT_READ, get_salt_ptr());
	return 0;
}

int key_derive_from_passphrase(const char *pass)
{
	unsigned int pass_len = strlen(pass);

	if (pass_len > MAX_PASSPHRASE_SIZE) {
		printf("Passphrase size is %d; max is %d.\n", pass_len,
		       MAX_PASSPHRASE_SIZE);
		exit(1);
	}

	/* Need to get salt from
	 * kernel first.
	 */
	if (read_salt())
		exit(1);
	/* Store the derived key in result buf. */
	pbkdf2_sha512(pass, get_salt_ptr(), PBKDF2_ITERATIONS, get_key_ptr());
	if (write_keys())
		exit(1);

	return 0;
}

void help(void)
{
	printf(
	"Usage: crypto_hibernate [OPTIONS]\n"
	"-p	passphrase [probed from user if not given]\n"
	"-s salt [read from kernel if not given]\n");
}

int main(int argc, char *argv[])
{
	int opt, option_index = 0;
	char in_passphrase[MAX_PASSPHRASE_SIZE];

	while ((opt = getopt_long_only(argc, argv, "+p:s:h",
				NULL, &option_index)) != -1) {
		switch (opt) {
		case 'p':
			{
				char *p = optarg;

				if (strlen(p) >= (MAX_PASSPHRASE_SIZE - 1)) {
					printf("Please provide passphrase less than %d bytes.\n",
						MAX_PASSPHRASE_SIZE);
					exit(1);
				}
				strcpy(in_passphrase, p);
			}
			break;
		case 's':
			{
				char *p = optarg;

				if (strlen(p) != (SALT_BYTES - 1)) {
					printf("Please provide salt with len less than %d bytes.\n",
						SALT_BYTES);
					exit(1);
				}
				strcpy(get_salt_ptr(), p);
			}
			break;
		case 'h':
		default:
			help();
			exit(1);
		}
	}

	printf("Enter passphrase (echo disabled): ");
	get_passphrase(in_passphrase, sizeof(in_passphrase));

	if (key_derive_from_passphrase(in_passphrase))
		exit(1);

	return 0;
}
