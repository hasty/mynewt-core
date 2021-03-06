/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <string.h>
#include <mcu/cmsis_nvic.h>
#include <os/mynewt.h>
#include <mcu/da1469x_clock.h>
#include <crypto/crypto.h>
#include <crypto_da1469x/crypto_da1469x.h>

static struct os_mutex gmtx;

#define VALID_AES_KEYLEN(x) (((x) == 128) || ((x) == 192) || ((x) == 256))

static bool
da1469x_has_support(struct crypto_dev *crypto, uint8_t op, uint16_t algo,
                    uint16_t mode, uint16_t keylen)
{
    (void)op;

    if (algo != CRYPTO_ALGO_AES || !VALID_AES_KEYLEN(keylen)) {
        return false;
    }

    switch (mode) {
    case CRYPTO_MODE_ECB:
    case CRYPTO_MODE_CBC:
    case CRYPTO_MODE_CTR:
        return true;
    }

    return false;
}

static uint32_t
da1469x_crypto_op(struct crypto_dev *crypto, uint8_t op, uint16_t algo,
                  uint16_t mode, const uint8_t *key, uint16_t keylen,
                  uint8_t *iv, const uint8_t *inbuf, uint8_t *outbuf,
                  uint32_t len)
{
    uint8_t i;
    uint8_t iv_save[AES_BLOCK_LEN];
    unsigned int inc;
    unsigned int carry;
    unsigned int v;
    uint32_t *keyreg;
    uint32_t *keyp32;
    uint32_t ctrl_reg;

    if (!da1469x_has_support(crypto, op, algo, mode, keylen)) {
        return 0;
    }

    if (mode == CRYPTO_MODE_CBC && op == CRYPTO_OP_DECRYPT) {
        memcpy(iv_save, &inbuf[len-AES_BLOCK_LEN], AES_BLOCK_LEN);
    }

    os_mutex_pend(&gmtx, OS_TIMEOUT_NEVER);
    da1469x_clock_amba_enable(CRG_TOP_CLK_AMBA_REG_AES_CLK_ENABLE_Msk);

    /* enable AES / disable HASH */
    ctrl_reg = ~AES_HASH_CRYPTO_CTRL_REG_CRYPTO_HASH_SEL_Msk;

    /* Select mode
     *
     * Note: DS also mentions value 1 for ECB.
     */
    ctrl_reg &= ~AES_HASH_CRYPTO_CTRL_REG_CRYPTO_ALG_MD_Msk;
    switch (mode) {
    case CRYPTO_MODE_ECB:
        ctrl_reg |= 0 << AES_HASH_CRYPTO_CTRL_REG_CRYPTO_ALG_MD_Pos;
        break;
    case CRYPTO_MODE_CTR:
        ctrl_reg |= 2 << AES_HASH_CRYPTO_CTRL_REG_CRYPTO_ALG_MD_Pos;
        break;
    case CRYPTO_MODE_CBC:
        ctrl_reg |= 3 << AES_HASH_CRYPTO_CTRL_REG_CRYPTO_ALG_MD_Pos;
        break;
    }

    /* AES algorithm, there's no need to check algo here! */
    ctrl_reg &= ~AES_HASH_CRYPTO_CTRL_REG_CRYPTO_ALG_Msk;

    /*
     * Note: DS also mentions value 3 for 256-bit key.
     */
    ctrl_reg &= ~AES_HASH_CRYPTO_CTRL_REG_CRYPTO_AES_KEY_SZ_Msk;
    switch (keylen) {
    case 128:
        ctrl_reg |= 0 << AES_HASH_CRYPTO_CTRL_REG_CRYPTO_AES_KEY_SZ_Pos;
        break;
    case 192:
        ctrl_reg |= 1 << AES_HASH_CRYPTO_CTRL_REG_CRYPTO_AES_KEY_SZ_Pos;
        break;
    case 256:
        ctrl_reg |= 2 << AES_HASH_CRYPTO_CTRL_REG_CRYPTO_AES_KEY_SZ_Pos;
        break;
    }

    /* activate key expansion */
    ctrl_reg |= AES_HASH_CRYPTO_CTRL_REG_CRYPTO_AES_KEXP_Msk;

    if (op == CRYPTO_OP_ENCRYPT) {
        ctrl_reg |= AES_HASH_CRYPTO_CTRL_REG_CRYPTO_ENCDEC_Msk;
    } else {
        ctrl_reg &= ~AES_HASH_CRYPTO_CTRL_REG_CRYPTO_ENCDEC_Msk;
    }

    /*
     * - no more data in
     * - disable IRQ
     * - write out all data
     */
    ctrl_reg &= ~(AES_HASH_CRYPTO_CTRL_REG_CRYPTO_MORE_IN_Msk |
                  AES_HASH_CRYPTO_CTRL_REG_CRYPTO_IRQ_EN_Msk |
                  AES_HASH_CRYPTO_CTRL_REG_CRYPTO_OUT_MD_Msk);

    AES_HASH->CRYPTO_CTRL_REG = ctrl_reg;

    switch (mode) {
    case CRYPTO_MODE_CBC:
    case CRYPTO_MODE_CTR:
        AES_HASH->CRYPTO_MREG0_REG = os_bswap_32(((uint32_t *)iv)[3]);
        AES_HASH->CRYPTO_MREG1_REG = os_bswap_32(((uint32_t *)iv)[2]);
        AES_HASH->CRYPTO_MREG2_REG = os_bswap_32(((uint32_t *)iv)[1]);
        AES_HASH->CRYPTO_MREG3_REG = os_bswap_32(((uint32_t *)iv)[0]);
    }

    keyreg = (uint32_t *)&AES_HASH->CRYPTO_KEYS_START;
    keyp32 = (uint32_t *)key;
    switch (keylen) {
    case 256:
        *keyreg++ = os_bswap_32(*keyp32); keyp32++;
        *keyreg++ = os_bswap_32(*keyp32); keyp32++;
        /* fallthrough */
    case 192:
        *keyreg++ = os_bswap_32(*keyp32); keyp32++;
        *keyreg++ = os_bswap_32(*keyp32); keyp32++;
        /* fallthrough */
    case 128:
    default:
        *keyreg++ = os_bswap_32(*keyp32); keyp32++;
        *keyreg++ = os_bswap_32(*keyp32); keyp32++;
        *keyreg++ = os_bswap_32(*keyp32); keyp32++;
        *keyreg++ = os_bswap_32(*keyp32); keyp32++;
    }

    AES_HASH->CRYPTO_FETCH_ADDR_REG = (uint32_t)inbuf;
    AES_HASH->CRYPTO_DEST_ADDR_REG = (uint32_t)outbuf;
    /* NOTE: length register allows only 24-bits */
    AES_HASH->CRYPTO_LEN_REG = len;

    /* start encryption/decryption */
    AES_HASH->CRYPTO_START_REG |= AES_HASH_CRYPTO_START_REG_CRYPTO_START_Msk;

    /*
     * Wait to finish.
     *
     * FIXME: refactor busy wait!?
     */
    while (!(AES_HASH->CRYPTO_STATUS_REG &
             AES_HASH_CRYPTO_STATUS_REG_CRYPTO_INACTIVE_Msk));

    da1469x_clock_amba_disable(CRG_TOP_CLK_AMBA_REG_AES_CLK_ENABLE_Msk);
    os_mutex_release(&gmtx);

    /*
     * Update crypto framework internals
     */
    switch (mode) {
    case CRYPTO_MODE_CBC:
        if (op == CRYPTO_OP_ENCRYPT) {
            memcpy(iv, &outbuf[len-AES_BLOCK_LEN], AES_BLOCK_LEN);
        } else {
            memcpy(iv, iv_save, AES_BLOCK_LEN);
        }
        break;
    case CRYPTO_MODE_CTR:
        inc = (len + AES_BLOCK_LEN - 1) / AES_BLOCK_LEN;
        carry = 0;
        for (i = AES_BLOCK_LEN; (inc != 0 || carry != 0) && i > 0; --i) {
            v = carry + (uint8_t)inc + iv[i - 1];
            iv[i - 1] = (uint8_t)v;
            inc >>= 8;
            carry = v >> 8;
        }
        break;
    }

    return len;
}

static uint32_t
da1469x_crypto_encrypt(struct crypto_dev *crypto, uint16_t algo, uint16_t mode,
                       const uint8_t *key, uint16_t keylen, uint8_t *iv,
                       const uint8_t *inbuf, uint8_t *outbuf, uint32_t len)
{
    return da1469x_crypto_op(crypto, CRYPTO_OP_ENCRYPT, algo, mode, key,
            keylen, iv, inbuf, outbuf, len);
}

static uint32_t
da1469x_crypto_decrypt(struct crypto_dev *crypto, uint16_t algo, uint16_t mode,
                       const uint8_t *key, uint16_t keylen, uint8_t *iv,
                       const uint8_t *inbuf, uint8_t *outbuf, uint32_t len)
{
    return da1469x_crypto_op(crypto, CRYPTO_OP_DECRYPT, algo, mode, key,
            keylen, iv, inbuf, outbuf, len);
}

int
da1469x_crypto_dev_init(struct os_dev *dev, void *arg)
{
    struct crypto_dev *crypto;
    os_error_t err;

    crypto = (struct crypto_dev *)dev;
    assert(crypto);

    OS_DEV_SETHANDLERS(dev, NULL, NULL);

    err = os_mutex_init(&gmtx);
    assert(err == OS_OK);

    crypto->interface.encrypt = da1469x_crypto_encrypt;
    crypto->interface.decrypt = da1469x_crypto_decrypt;
    crypto->interface.has_support = da1469x_has_support;

    return 0;
}
