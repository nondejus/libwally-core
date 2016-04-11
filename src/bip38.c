#include <include/wally-core.h>
/*#include <include/wally_bip38.h>*/
#include "internal.h"
#include "base58.h"
#include "scrypt.h"
#include "ccan/ccan/crypto/sha256/sha256.h"
#include "ccan/ccan/crypto/ripemd160/ripemd160.h"
#include "ccan/ccan/endian/endian.h"
#include "ccan/ccan/build_assert/build_assert.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#include "ctaes/ctaes.h"
#include "ctaes/ctaes.c"

#define BIP38_FLAG_DEFAULT   (0x40 | 0x80)
#define BIP38_FLAG_COMPRESSED 0x20
#define BIP38_FLAG_RESERVED1  0x10
#define BIP38_FLAG_RESERVED2  0x08
#define BIP38_FLAG_HAVE_LOT   0x04
#define BIP38_FLAG_RESERVED3  0x02
#define BIP38_FLAG_RESERVED4  0x01
#define BIP38_FLAGS_RESERVED (BIP38_FLAG_RESERVED1 | BIP38_FLAG_RESERVED2 | \
                              BIP38_FLAG_RESERVED3 | BIP38_FLAG_RESERVED4)

#define BITCOIN_PRIVATE_KEY_LEN 32
#define BIP38_DERVIED_KEY_LEN 64u
#define AES256_BLOCK_LEN 16u

/* FIXME: Share this with key_compute_pub_key in bip32.c */
static int compute_pub_key(const unsigned char *priv_key, size_t priv_len,
                           unsigned char *pub_key_out, bool compressed)
{
    secp256k1_pubkey pk;
    const secp256k1_context *ctx = secp_ctx();
    unsigned int flags = compressed ? PUBKEY_COMPRESSED : PUBKEY_UNCOMPRESSED;
    size_t len = compressed ? 33 : 65;
    int ret = priv_len == BITCOIN_PRIVATE_KEY_LEN &&
              pubkey_create(ctx, &pk, priv_key) &&
              pubkey_serialize(ctx, pub_key_out, &len, &pk, flags) ? 0 : -1;
    clear(&pk, sizeof(pk));
    return ret;
}


static int address_from_private_key(unsigned char *priv_key,
                                    size_t priv_len,
                                    unsigned char network,
                                    bool compressed,
                                    char **output)
{
    struct sha256 sha;
    unsigned char pub_key[65];
    struct
    {
        unsigned char pad1[3];
        unsigned char network;
        struct ripemd160 hash160;
        uint32_t checksum;
    } buf;
    int ret;

    BUILD_ASSERT(&buf.network + 1 == (void *)&buf.hash160);

    if (compute_pub_key(priv_key, priv_len, pub_key, compressed))
        return -1;

    sha256(&sha, pub_key, compressed ? 33 : 65);
    ripemd160(&buf.hash160, &sha, sizeof(sha));
    buf.network = network;
    buf.checksum = base58_get_checksum(&buf.network, 1 + 20);
    ret = base58_from_bytes(&buf.network, 1 + 20 + 4,
                            BASE58_FLAG_CHECKSUM, output);
    clear_n(3, &sha, sizeof(sha), pub_key, sizeof(pub_key), &buf, sizeof(buf));
    return ret;
}

static void aes_inc(unsigned char *block_in_out, const unsigned char *xor,
                    const unsigned char *key, unsigned char *bytes_out)
{
    AES256_ctx ctx;
    size_t i;

    memset(bytes_out, 0, AES256_BLOCK_LEN);
    for (i = 0; i < AES256_BLOCK_LEN; ++i)
        block_in_out[i] ^= xor[i];

    AES256_init(&ctx, key);
    AES256_encrypt(&ctx, 1, bytes_out, block_in_out);
    clear(&ctx, sizeof(ctx));
}

int bip38_from_private_key(unsigned char *priv_key, size_t len,
                           const unsigned char *pass, size_t pass_len,
                           unsigned char network, bool compressed,
                           char **output)
{
    unsigned char derived[BIP38_DERVIED_KEY_LEN];
    unsigned char buf[7 + AES256_BLOCK_LEN * 2];
    uint32_t hash;
    char *addr58 = NULL;
    int ret = -1;

    *output = NULL;

    if (address_from_private_key(priv_key, len, network, compressed, &addr58))
        goto finish;

    hash = base58_get_checksum((unsigned char *)addr58, strlen(addr58));
    if (scrypt(pass, pass_len, (unsigned char *)&hash, sizeof(hash),
               16384, 8, 8, derived, sizeof(derived)))
        goto finish;

    buf[0] = 0x01;
    buf[1] = 0x42; /* FIXME: EC-Multiply support */
    buf[2] = BIP38_FLAG_DEFAULT | (compressed ? BIP38_FLAG_COMPRESSED : 0);
    memcpy(buf + 3, &hash, sizeof(hash));
    aes_inc(priv_key + 0, derived + 0, derived + 32, buf + 7 + 0);
    aes_inc(priv_key + 16, derived + 16, derived + 32, buf + 7 + 16);
    ret = base58_from_bytes(buf, sizeof(buf), BASE58_FLAG_CHECKSUM, output);

finish:
    wally_free_string(addr58);
    clear_n(3, derived, sizeof(derived), buf, sizeof(buf), &hash, sizeof(hash));
    return ret;
}