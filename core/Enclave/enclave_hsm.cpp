/*
 * Copyright (C) 2020-2021 Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in
 *      the documentation and/or other materials provided with the
 *      distribution.
 *   3. Neither the name of Intel Corporation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "enclave_hsm_t.h"
#include "openssl/rand.h"
#include "datatypes.h"
#include "key_factory.h"
#include "key_operation.h"

using namespace std;

// Used to store the secret passed by the SP in the sample code.

static const sgx_ec256_public_t g_sp_pub_key = {
    {0x72, 0x12, 0x8a, 0x7a, 0x17, 0x52, 0x6e, 0xbf,
     0x85, 0xd0, 0x3a, 0x62, 0x37, 0x30, 0xae, 0xad,
     0x3e, 0x3d, 0xaa, 0xee, 0x9c, 0x60, 0x73, 0x1d,
     0xb0, 0x5b, 0xe8, 0x62, 0x1c, 0x4b, 0xeb, 0x38},
    {0xd4, 0x81, 0x40, 0xd9, 0x50, 0xe2, 0x57, 0x7b,
     0x26, 0xee, 0xb7, 0x41, 0xe7, 0xc6, 0x14, 0xe2,
     0x24, 0xb7, 0xbd, 0xc9, 0x03, 0xf2, 0x9a, 0x28,
     0xa8, 0x3c, 0xc8, 0x10, 0x11, 0x14, 0x5e, 0x06}

};

static uint32_t get_asymmetric_max_encrypt_plaintext_size(ehsm_keyspec_t keyspec, ehsm_padding_mode_t padding)
{
    uint32_t padding_size;
    switch (padding)
    {
    case EH_PAD_RSA_PKCS1:
        padding_size = 11;
        break;
    case EH_PAD_RSA_PKCS1_OAEP:
        // https://github.com/openssl/openssl/blob/OpenSSL_1_1_1-stable/crypto/rsa/rsa_oaep.c#L66
        padding_size = 42;
        break;
    case EH_PAD_RSA_NO:
    default:
        padding_size = 0;
        break;
    }
    switch (keyspec)
    {
    case EH_RSA_2048:
        return 256 - padding_size;
    case EH_RSA_3072:
        return 384 - padding_size;
    case EH_RSA_4096:
        return 512 - padding_size;
    case EH_SM2:
        // https://github.com/guanzhi/GmSSL/blob/v3.0.0/include/gmssl/sm2.h#L345
        return 255;
    default:
        return 0;
    }
}

static size_t get_signature_length(ehsm_keyspec_t keyspec)
{
    switch (keyspec)
    {
    case EH_RSA_2048:
        return RSA_OAEP_2048_SIGNATURE_SIZE;
    case EH_RSA_3072:
        return RSA_OAEP_3072_SIGNATURE_SIZE;
    case EH_RSA_4096:
        return RSA_OAEP_4096_SIGNATURE_SIZE;
    case EH_EC_P256:
        return EC_P256_SIGNATURE_MAX_SIZE;
    case EH_EC_P224:
        return EC_P224_SIGNATURE_MAX_SIZE;
    case EH_EC_P384:
        return EC_P384_SIGNATURE_MAX_SIZE;
    case EH_EC_P521:
        return EC_P521_SIGNATURE_MAX_SIZE;
    case EH_SM2:
        return EC_SM2_SIGNATURE_MAX_SIZE;
    default:
        return -1;
    }
}

sgx_status_t enclave_create_key(ehsm_keyblob_t *cmk, size_t cmk_size)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    if (cmk == NULL ||
        cmk_size != APPEND_SIZE_TO_KEYBLOB_T(cmk->keybloblen) ||
        cmk->metadata.origin != EH_INTERNAL_KEY)
    {
        return SGX_ERROR_INVALID_PARAMETER;
    }

    switch (cmk->metadata.keyspec)
    {
    case EH_AES_GCM_128:
    case EH_AES_GCM_192:
    case EH_AES_GCM_256:
        ret = ehsm_create_aes_key(cmk);
        break;
    case EH_RSA_2048:
    case EH_RSA_3072:
    case EH_RSA_4096:
        ret = ehsm_create_rsa_key(cmk);
        break;
    case EH_EC_P224:
    case EH_EC_P256:
    case EH_EC_P384:
    case EH_EC_P521:
        ret = ehsm_create_ecc_key(cmk);
        break;
    case EH_SM2:
        ret = ehsm_create_sm2_key(cmk);
        break;
    case EH_SM4_CTR:
    case EH_SM4_CBC:
        ret = ehsm_create_sm4_key(cmk);
        break;
    default:
        ret = SGX_ERROR_INVALID_PARAMETER;
    }

    return ret;
}

sgx_status_t enclave_encrypt(ehsm_keyblob_t *cmk, size_t cmk_size,
                             ehsm_data_t *aad, size_t aad_size,
                             ehsm_data_t *plaintext, size_t plaintext_size,
                             ehsm_data_t *ciphertext, size_t ciphertext_size)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    if (cmk == NULL ||
        cmk_size != APPEND_SIZE_TO_KEYBLOB_T(cmk->keybloblen) ||
        cmk->keybloblen == 0 ||
        cmk->metadata.origin != EH_INTERNAL_KEY)
        return SGX_ERROR_INVALID_PARAMETER;

    if (aad != NULL && aad_size != APPEND_SIZE_TO_DATA_T(aad->datalen))
        return SGX_ERROR_INVALID_PARAMETER;

    if (aad == NULL && aad_size != 0)
        return SGX_ERROR_INVALID_PARAMETER;

    /* only support to directly encrypt data of less than 6 KB */
    if (plaintext == NULL ||
        plaintext_size != APPEND_SIZE_TO_DATA_T(plaintext->datalen) ||
        plaintext->datalen == 0 ||
        plaintext->datalen > EH_ENCRYPT_MAX_SIZE)
        return SGX_ERROR_INVALID_PARAMETER;

    if (ciphertext == NULL ||
        ciphertext_size != APPEND_SIZE_TO_DATA_T(ciphertext->datalen))
        return SGX_ERROR_INVALID_PARAMETER;

    switch (cmk->metadata.keyspec)
    {
    case EH_AES_GCM_128:
    case EH_AES_GCM_192:
    case EH_AES_GCM_256:
        ret = ehsm_aes_gcm_encrypt(aad, cmk, plaintext, ciphertext);
        break;
    case EH_SM4_CTR:
        ret = ehsm_sm4_ctr_encrypt(cmk, plaintext, ciphertext);
        break;
    case EH_SM4_CBC:
        ret = ehsm_sm4_cbc_encrypt(cmk, plaintext, ciphertext);
        break;
    default:
        return SGX_ERROR_INVALID_PARAMETER;
    }

    return ret;
}

sgx_status_t enclave_decrypt(ehsm_keyblob_t *cmk, size_t cmk_size,
                             ehsm_data_t *aad, size_t aad_size,
                             ehsm_data_t *ciphertext, size_t ciphertext_size,
                             ehsm_data_t *plaintext, size_t plaintext_size)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    if (cmk == NULL ||
        cmk_size != APPEND_SIZE_TO_KEYBLOB_T(cmk->keybloblen) ||
        cmk->keybloblen == 0 ||
        cmk->metadata.origin != EH_INTERNAL_KEY)
        return SGX_ERROR_INVALID_PARAMETER;

    if (aad != NULL && aad_size != APPEND_SIZE_TO_DATA_T(aad->datalen))
        return SGX_ERROR_INVALID_PARAMETER;

    if (aad == NULL && aad_size != 0)
        return SGX_ERROR_INVALID_PARAMETER;

    if (plaintext == NULL ||
        plaintext_size != APPEND_SIZE_TO_DATA_T(plaintext->datalen))
        return SGX_ERROR_INVALID_PARAMETER;

    if (ciphertext == NULL ||
        ciphertext_size != APPEND_SIZE_TO_DATA_T(ciphertext->datalen) ||
        ciphertext->datalen == 0)
        return SGX_ERROR_INVALID_PARAMETER;

    switch (cmk->metadata.keyspec)
    {
    case EH_AES_GCM_128:
    case EH_AES_GCM_192:
    case EH_AES_GCM_256:
        ret = ehsm_aes_gcm_decrypt(aad, cmk, ciphertext, plaintext);
        break;
    case EH_SM4_CTR:
        ret = ehsm_sm4_ctr_decrypt(cmk, ciphertext, plaintext);
        break;
    case EH_SM4_CBC:
        ret = ehsm_sm4_cbc_decrypt(cmk, ciphertext, plaintext);
        break;
    default:
        return SGX_ERROR_INVALID_PARAMETER;
    }

    return ret;
}

sgx_status_t enclave_asymmetric_encrypt(const ehsm_keyblob_t *cmk, size_t cmk_size,
                                        ehsm_data_t *plaintext, size_t plaintext_size,
                                        ehsm_data_t *ciphertext, size_t ciphertext_size)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    if (cmk == NULL ||
        cmk_size != APPEND_SIZE_TO_KEYBLOB_T(cmk->keybloblen) ||
        cmk->keybloblen == 0 ||
        cmk->metadata.origin != EH_INTERNAL_KEY)
        return SGX_ERROR_INVALID_PARAMETER;

    if (plaintext == NULL ||
        plaintext_size != APPEND_SIZE_TO_DATA_T(plaintext->datalen) ||
        plaintext->datalen == 0 ||
        /* Verify the maximum plaintext length supported by different keyspac */
        plaintext->datalen > get_asymmetric_max_encrypt_plaintext_size(cmk->metadata.keyspec, cmk->metadata.padding_mode))
        return SGX_ERROR_INVALID_PARAMETER;

    if (ciphertext == NULL ||
        ciphertext_size != APPEND_SIZE_TO_DATA_T(ciphertext->datalen))
        return SGX_ERROR_INVALID_PARAMETER;

    switch (cmk->metadata.keyspec)
    {
    case EH_RSA_2048:
    case EH_RSA_3072:
    case EH_RSA_4096:
        ret = ehsm_rsa_encrypt(cmk, plaintext, ciphertext);
        break;
    case EH_SM2:
        ret = ehsm_sm2_encrypt(cmk, plaintext, ciphertext);
        break;
    default:
        return SGX_ERROR_INVALID_PARAMETER;
    }
    return ret;
}

sgx_status_t enclave_asymmetric_decrypt(const ehsm_keyblob_t *cmk, size_t cmk_size,
                                        ehsm_data_t *ciphertext, uint32_t ciphertext_size,
                                        ehsm_data_t *plaintext, uint32_t plaintext_size)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    if (cmk == NULL ||
        cmk_size != APPEND_SIZE_TO_KEYBLOB_T(cmk->keybloblen) ||
        cmk->keybloblen == 0 ||
        cmk->metadata.origin != EH_INTERNAL_KEY)
        return SGX_ERROR_INVALID_PARAMETER;

    if (plaintext == NULL ||
        plaintext_size != APPEND_SIZE_TO_DATA_T(plaintext->datalen))
        return SGX_ERROR_INVALID_PARAMETER;

    if (ciphertext == NULL ||
        ciphertext_size != APPEND_SIZE_TO_DATA_T(ciphertext->datalen) ||
        ciphertext->datalen == 0)
        return SGX_ERROR_INVALID_PARAMETER;

    switch (cmk->metadata.keyspec)
    {
    case EH_RSA_2048:
    case EH_RSA_3072:
    case EH_RSA_4096:
        ret = ehsm_rsa_decrypt(cmk, ciphertext, plaintext);
        break;
    case EH_SM2:
        ret = ehsm_sm2_decrypt(cmk, ciphertext, plaintext);
        break;
    default:
        return SGX_ERROR_INVALID_PARAMETER;
    }
    return ret;
}

/**
 * @brief Sign the message and store it in signature
 *
 * @param cmk storage the key metadata and keyblob
 * @param cmk_size size of input cmk
 * @param data message to be signed
 * @param data_size size of input message
 * @param signature generated signature
 * @param signature_size size of input signature
 * @return ehsm_status_t
 */
sgx_status_t enclave_sign(const ehsm_keyblob_t *cmk, size_t cmk_size,
                          const ehsm_data_t *data, size_t data_size,
                          ehsm_data_t *signature, size_t signature_size)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    // check cmk_blob and cmk_blob_size
    if (cmk == NULL ||
        cmk_size != APPEND_SIZE_TO_KEYBLOB_T(cmk->keybloblen) ||
        cmk->keybloblen == 0 ||
        cmk->metadata.origin != EH_INTERNAL_KEY)
        return SGX_ERROR_INVALID_PARAMETER;

    if (signature == NULL ||
        signature_size != APPEND_SIZE_TO_DATA_T(signature->datalen))
    {
        log_d("ecall sign signture or signature_size wrong.\n");
        return SGX_ERROR_INVALID_PARAMETER;
    }
    // Set signature data length
    if (signature->datalen == 0)
    {
        signature->datalen = get_signature_length(cmk->metadata.keyspec);
        return SGX_SUCCESS;
    }
    if (signature->datalen == -1 ||
        signature->datalen != get_signature_length(cmk->metadata.keyspec))
    {
        log_d("ecall sign cant get signature length or ecall sign signature length error.\n");
        return SGX_ERROR_INVALID_PARAMETER;
    }
    if (data == NULL ||
        data_size != APPEND_SIZE_TO_DATA_T(data->datalen) ||
        data->datalen == 0)
    {
        log_d("ecall sign data or data len is wrong.\n");
        return SGX_ERROR_INVALID_PARAMETER;
    }

    switch (cmk->metadata.keyspec)
    {
    case EH_RSA_2048:
    case EH_RSA_3072:
    case EH_RSA_4096:
        ret = ehsm_rsa_sign(cmk,
                            data,
                            signature);
        break;
    case EH_EC_P256:
    case EH_EC_P224:
    case EH_EC_P384:
    case EH_EC_P521:
        ret = ehsm_ecc_sign(cmk,
                            data,
                            signature);
        break;
    case EH_SM2:
        ret = ehsm_sm2_sign(cmk,
                            data,
                            signature);
        break;
    default:
        log_d("ecall sign unsupport keyspec.\n");
        return SGX_ERROR_INVALID_PARAMETER;
    }

    return ret;
}

/**
 * @brief verify the signature is correct
 *
 * @param cmk storage the key metadata and keyblob
 * @param cmk_size size of input cmk
 * @param data message for signature
 * @param data_size size of input message
 * @param signature generated signature
 * @param signature_size size of input signature
 * @param result Signature match result
 * @return ehsm_status_t
 */
sgx_status_t enclave_verify(const ehsm_keyblob_t *cmk, size_t cmk_size,
                            const ehsm_data_t *data, size_t data_size,
                            const ehsm_data_t *signature, size_t signature_size,
                            bool *result)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    if (cmk == NULL ||
        cmk_size != APPEND_SIZE_TO_KEYBLOB_T(cmk->keybloblen) ||
        cmk->keybloblen == 0 ||
        cmk->metadata.origin != EH_INTERNAL_KEY)
        return SGX_ERROR_INVALID_PARAMETER;

    if (signature == NULL ||
        signature_size != APPEND_SIZE_TO_DATA_T(signature->datalen) ||
        signature->datalen <= 0)
    {
        log_d("ecall verify signture or signature_size wrong.\n");
        return SGX_ERROR_INVALID_PARAMETER;
    }
    if (data == NULL ||
        data_size != APPEND_SIZE_TO_DATA_T(data->datalen) ||
        data->datalen == 0)
    {
        log_d("ecall verify data or data len is wrong.\n");
        return SGX_ERROR_INVALID_PARAMETER;
    }
    if (result == NULL)
    {
        log_d("ecall verify result is NULL.\n");
        return SGX_ERROR_INVALID_PARAMETER;
    }

    switch (cmk->metadata.keyspec)
    {
    case EH_RSA_2048:
    case EH_RSA_3072:
    case EH_RSA_4096:
        if (signature->datalen != get_signature_length(cmk->metadata.keyspec))
        {
            log_d("ecall verify cant get signature length or ecall sign signature length error.\n");
            return SGX_ERROR_INVALID_PARAMETER;
        }
        ret = ehsm_rsa_verify(cmk,
                              data,
                              signature,
                              result);
        break;
    case EH_EC_P256:
    case EH_EC_P224:
    case EH_EC_P384:
    case EH_EC_P521:
        // not check ecc & sm2 signateure len because the len will be change after sign
        // refence https://wiki.openssl.org/index.php/EVP_Signing_and_Verifying#Signing
        ret = ehsm_ecc_verify(cmk,
                              data,
                              signature,
                              result);
        break;
    case EH_SM2:
        ret = ehsm_sm2_verify(cmk,
                              data,
                              signature,
                              result);
        break;
    default:
        log_d("ecall verify unsupport keyspec.\n");
        return SGX_ERROR_INVALID_PARAMETER;
    }

    return ret;
}

/**
 * @brief verify the signature is correct
 *
 * @param cmk storage the key metadata and keyblob
 * @param cmk_size size of input cmk
 * @param aad additional data
 * @param aad_size size of additional data
 * @param plaintext data to be encrypted
 * @param plaintext_size size of data to be encrypted
 * @param ciphertext information of ciphertext
 * @param ciphertext_size size of ciphertext
 * @return ehsm_status_t
 */
sgx_status_t enclave_generate_datakey(ehsm_keyblob_t *cmk, size_t cmk_size,
                                      ehsm_data_t *aad, size_t aad_size,
                                      ehsm_data_t *plaintext, size_t plaintext_size,
                                      ehsm_data_t *ciphertext, size_t ciphertext_size)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;

    if (cmk == NULL ||
        cmk_size != APPEND_SIZE_TO_KEYBLOB_T(cmk->keybloblen) ||
        cmk->keybloblen == 0 ||
        cmk->metadata.origin != EH_INTERNAL_KEY)
        return SGX_ERROR_INVALID_PARAMETER;

    if (plaintext == NULL ||
        plaintext_size != APPEND_SIZE_TO_DATA_T(plaintext->datalen) ||
        plaintext->datalen > 1024 ||
        plaintext->datalen == 0)
        return SGX_ERROR_INVALID_PARAMETER;

    if (aad != NULL && aad_size != APPEND_SIZE_TO_DATA_T(aad->datalen))
        return SGX_ERROR_INVALID_PARAMETER;

    if (aad == NULL && aad_size != 0)
        return SGX_ERROR_INVALID_PARAMETER;

    if (ciphertext == NULL ||
        ciphertext_size != APPEND_SIZE_TO_DATA_T(ciphertext->datalen))
        return SGX_ERROR_INVALID_PARAMETER;

    if (ciphertext->datalen == 0)
    {
        switch (cmk->metadata.keyspec)
        {
        case EH_AES_GCM_128:
        case EH_AES_GCM_192:
        case EH_AES_GCM_256:
            ciphertext->datalen = plaintext->datalen + EH_AES_GCM_IV_SIZE + EH_AES_GCM_MAC_SIZE;
            return SGX_SUCCESS;
        case EH_SM4_CBC:
            if (plaintext->datalen % 16 != 0)
            {
                ciphertext->datalen = (plaintext->datalen / 16 + 1) * 16 + SGX_SM4_IV_SIZE;
                return SGX_SUCCESS;
            }
            ciphertext->datalen = plaintext->datalen + SGX_SM4_IV_SIZE;
            return SGX_SUCCESS;
        case EH_SM4_CTR:
            ciphertext->datalen = plaintext->datalen + SGX_SM4_IV_SIZE;
            return SGX_SUCCESS;
        default:
            return SGX_ERROR_INVALID_PARAMETER;
        }
    }

    uint8_t *temp_datakey = NULL;

    temp_datakey = (uint8_t *)malloc(plaintext->datalen);
    if (temp_datakey == NULL)
        return SGX_ERROR_OUT_OF_MEMORY;

    if (sgx_read_rand(temp_datakey, plaintext->datalen) != SGX_SUCCESS)
    {
        free(temp_datakey);
        return SGX_ERROR_OUT_OF_MEMORY;
    }

    memcpy_s(plaintext->data, plaintext->datalen, temp_datakey, plaintext->datalen);

    switch (cmk->metadata.keyspec)
    {
    case EH_AES_GCM_128:
    case EH_AES_GCM_192:
    case EH_AES_GCM_256:
        ret = ehsm_aes_gcm_encrypt(aad,
                                   cmk,
                                   plaintext,
                                   ciphertext);
        break;
    case EH_SM4_CBC:
        ret = ehsm_sm4_cbc_encrypt(cmk,
                                   plaintext,
                                   ciphertext);
        break;
    case EH_SM4_CTR:
        ret = ehsm_sm4_ctr_encrypt(cmk,
                                   plaintext,
                                   ciphertext);
        break;
    default:
        break;
    }
    memset_s(temp_datakey, plaintext->datalen, 0, plaintext->datalen);
    free(temp_datakey);
    return ret;
}

sgx_status_t enclave_export_datakey(ehsm_keyblob_t *cmk, size_t cmk_size,
                                    ehsm_data_t *aad, size_t aad_size,
                                    ehsm_data_t *olddatakey, size_t olddatakey_size,
                                    ehsm_keyblob_t *ukey, size_t ukey_size,
                                    ehsm_data_t *newdatakey, size_t newdatakey_size)
{
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;
    if (cmk == NULL ||
        cmk_size != APPEND_SIZE_TO_KEYBLOB_T(cmk->keybloblen) ||
        cmk->keybloblen == 0 ||
        cmk->metadata.origin != EH_INTERNAL_KEY)
        return SGX_ERROR_INVALID_PARAMETER;

    if (aad != NULL && aad_size != APPEND_SIZE_TO_DATA_T(aad->datalen))
        return SGX_ERROR_INVALID_PARAMETER;

    if (aad == NULL && aad_size != 0)
        return SGX_ERROR_INVALID_PARAMETER;

    if (olddatakey == NULL ||
        olddatakey_size != APPEND_SIZE_TO_DATA_T(olddatakey->datalen))
        return SGX_ERROR_INVALID_PARAMETER;

    if (ukey == NULL ||
        ukey_size != APPEND_SIZE_TO_KEYBLOB_T(ukey->keybloblen) ||
        ukey->keyblob == NULL)
        return SGX_ERROR_INVALID_PARAMETER;

    if (newdatakey == NULL ||
        newdatakey_size != APPEND_SIZE_TO_DATA_T(newdatakey->datalen))
        return SGX_ERROR_INVALID_PARAMETER;

    ehsm_data_t *tmp_datakey = NULL;
    size_t tmp_datakey_size = 0;
    tmp_datakey = (ehsm_data_t *)malloc(APPEND_SIZE_TO_DATA_T(0));
    if (tmp_datakey == NULL)
    {
        ret = SGX_ERROR_INVALID_PARAMETER;
        goto out;
    }

    // datakey plaintext
    // to calc the plaintext len
    switch (cmk->metadata.keyspec)
    {
    case EH_AES_GCM_128:
    case EH_AES_GCM_192:
    case EH_AES_GCM_256:
        tmp_datakey->datalen = olddatakey->datalen - EH_AES_GCM_IV_SIZE - EH_AES_GCM_MAC_SIZE;
        tmp_datakey_size = APPEND_SIZE_TO_DATA_T(tmp_datakey->datalen);
        break;
    case EH_SM4_CBC:
    case EH_SM4_CTR:
        tmp_datakey->datalen = olddatakey->datalen - SGX_SM4_IV_SIZE;
        tmp_datakey_size = APPEND_SIZE_TO_DATA_T(tmp_datakey->datalen);
        break;
    default:
        ret = SGX_ERROR_INVALID_PARAMETER;
        goto out;
    }
    tmp_datakey = (ehsm_data_t *)realloc(tmp_datakey, APPEND_SIZE_TO_DATA_T(tmp_datakey->datalen));
    if (tmp_datakey == NULL)
    {
        tmp_datakey_size = 0;
        ret = SGX_ERROR_INVALID_PARAMETER;
        goto out;
    }
    // decrypt olddatakey using cmk
    switch (cmk->metadata.keyspec)
    {
    case EH_AES_GCM_128:
    case EH_AES_GCM_192:
    case EH_AES_GCM_256:
        ret = enclave_decrypt(cmk, cmk_size, aad, aad_size, olddatakey, olddatakey_size, tmp_datakey, tmp_datakey_size);
        break;
    case EH_SM4_CBC:
    case EH_SM4_CTR:
        ret = enclave_decrypt(cmk, cmk_size, aad, aad_size, olddatakey, olddatakey_size, tmp_datakey, tmp_datakey_size);
        break;
    default:
        ret = SGX_ERROR_INVALID_PARAMETER;
        goto out;
    }
    // check enclave_decrypt status
    if (ret != SGX_SUCCESS)
        goto out;

    // calc length
    // encrypt datakey using ukey
    // or just ret = enclave_asymmetric_encrypt(ukey, ukey_size, tmp_datakey, tmp_datakey_size, newdatakey, newdatakey_size);
    switch (ukey->metadata.keyspec)
    {
    case EH_RSA_2048:
    case EH_RSA_3072:
    case EH_RSA_4096:
    case EH_SM2:
        ret = enclave_asymmetric_encrypt(ukey, ukey_size, tmp_datakey, tmp_datakey_size, newdatakey, newdatakey_size);
        break;
    default:
        ret = SGX_ERROR_INVALID_PARAMETER;
        goto out;
    }
    if (ret != SGX_SUCCESS)
        goto out;

out:
    if (tmp_datakey_size != 0)
        memset_s(tmp_datakey, tmp_datakey_size, 0, tmp_datakey_size);

    SAFE_FREE(tmp_datakey);
    return ret;
}

sgx_status_t enclave_get_target_info(sgx_target_info_t *target_info)
{
    return sgx_self_target(target_info);
}

sgx_status_t enclave_create_report(const sgx_target_info_t *p_qe3_target, sgx_report_t *p_report)
{
    sgx_status_t ret = SGX_SUCCESS;

    sgx_report_data_t report_data = {0};

    // Generate the report for the app_enclave
    ret = sgx_create_report(p_qe3_target, &report_data, p_report);

    return ret;
}

sgx_status_t enclave_get_rand(uint8_t *data, uint32_t datalen)
{
    if (data == NULL)
        return SGX_ERROR_INVALID_PARAMETER;

    return sgx_read_rand(data, datalen);
}

sgx_status_t enclave_generate_apikey(sgx_ra_context_t context,
                                     uint8_t *p_apikey, uint32_t apikey_size,
                                     uint8_t *cipherapikey, uint32_t cipherapikey_size)
{
    sgx_status_t ret = SGX_SUCCESS;
    if (p_apikey == NULL || apikey_size > EH_API_KEY_SIZE)
    {
        return SGX_ERROR_INVALID_PARAMETER;
    }
    if (cipherapikey == NULL ||
        cipherapikey_size < EH_API_KEY_SIZE + EH_AES_GCM_IV_SIZE + EH_AES_GCM_MAC_SIZE)
    {
        return SGX_ERROR_INVALID_PARAMETER;
    }

    // generate apikey
    std::string psw_chars = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz";
    uint8_t temp[apikey_size];
    ret = sgx_read_rand(temp, apikey_size);
    if (ret != SGX_SUCCESS)
    {
        return ret;
    }
    for (int i = 0; i < apikey_size; i++)
    {
        p_apikey[i] = psw_chars[temp[i] % psw_chars.length()];
    }

    // struct cipherapikey{
    //     uint8_t apikey[32]
    //     uint8_t iv[12]
    //     uint8_t mac[16]
    // }
    uint8_t *iv = (uint8_t *)(cipherapikey + apikey_size);
    uint8_t *mac = (uint8_t *)(cipherapikey + apikey_size + EH_AES_GCM_IV_SIZE);
    // get sk and encrypt apikey
    sgx_ec_key_128bit_t sk_key;
    ret = sgx_ra_get_keys(context, SGX_RA_KEY_SK, &sk_key);
    if (ret != SGX_SUCCESS)
    {
        return ret;
    }
    ret = sgx_rijndael128GCM_encrypt(&sk_key,
                                     p_apikey, apikey_size,
                                     cipherapikey,
                                     iv, EH_AES_GCM_IV_SIZE,
                                     NULL, 0,
                                     reinterpret_cast<uint8_t(*)[EH_AES_GCM_MAC_SIZE]>(mac));
    if (ret != SGX_SUCCESS)
    {
        log_d("error encrypting plain text\n");
    }
    memset_s(sk_key, sizeof(sgx_ec_key_128bit_t), 0, sizeof(sgx_ec_key_128bit_t));
    memset_s(temp, apikey_size, 0, apikey_size);
    return ret;
}

sgx_status_t enclave_get_apikey(uint8_t *apikey, uint32_t keylen)
{
    sgx_status_t ret = SGX_SUCCESS;
    if (apikey == NULL || keylen != EH_API_KEY_SIZE)
    {
        return SGX_ERROR_INVALID_PARAMETER;
    }

    // generate apikey
    std::string psw_chars = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz";
    uint8_t temp[keylen];
    ret = sgx_read_rand(temp, keylen);
    if (ret != SGX_SUCCESS)
    {
        return ret;
    }
    for (int i = 0; i < keylen; i++)
    {
        apikey[i] = psw_chars[temp[i] % psw_chars.length()];
    }

    memset_s(temp, keylen, 0, keylen);
    return ret;
}
// This ecall is a wrapper of sgx_ra_init to create the trusted
// KE exchange key context needed for the remote attestation
// SIGMA API's. Input pointers aren't checked since the trusted stubs
// copy them into EPC memory.
//
// @param b_pse Indicates whether the ISV app is using the
//              platform services.
// @param p_context Pointer to the location where the returned
//                  key context is to be copied.
//
// @return Any error returned from the trusted key exchange API
//         for creating a key context.

sgx_status_t enclave_init_ra(
    int b_pse,
    sgx_ra_context_t *p_context)
{
    // isv enclave call to trusted key exchange library.
    sgx_status_t ret;
#ifdef SUPPLIED_KEY_DERIVATION
    ret = sgx_ra_init_ex(&g_sp_pub_key, b_pse, key_derivation, p_context);
#else
    ret = sgx_ra_init(&g_sp_pub_key, b_pse, p_context);
#endif
    return ret;
}

// Verify the mac sent in att_result_msg from the SP using the
// MK key. Input pointers aren't checked since the trusted stubs
// copy them into EPC memory.
//
//
// @param context The trusted KE library key context.
// @param p_message Pointer to the message used to produce MAC
// @param message_size Size in bytes of the message.
// @param p_mac Pointer to the MAC to compare to.
// @param mac_size Size in bytes of the MAC
//
// @return SGX_ERROR_INVALID_PARAMETER - MAC size is incorrect.
// @return Any error produced by tKE  API to get SK key.
// @return Any error produced by the AESCMAC function.
// @return SGX_ERROR_MAC_MISMATCH - MAC compare fails.

sgx_status_t enclave_verify_att_result_mac(sgx_ra_context_t context,
                                           uint8_t *p_message, size_t message_size,
                                           uint8_t *p_mac, size_t mac_size)
{
    sgx_status_t ret;
    sgx_ec_key_128bit_t mk_key;

    if (mac_size != sizeof(sgx_mac_t))
    {
        ret = SGX_ERROR_INVALID_PARAMETER;
        return ret;
    }
    if (message_size > UINT32_MAX)
    {
        ret = SGX_ERROR_INVALID_PARAMETER;
        return ret;
    }

    do
    {
        uint8_t mac[SGX_CMAC_MAC_SIZE] = {0};

        ret = sgx_ra_get_keys(context, SGX_RA_KEY_MK, &mk_key);
        if (SGX_SUCCESS != ret)
            break;

        ret = sgx_rijndael128_cmac_msg(&mk_key,
                                       p_message,
                                       (uint32_t)message_size,
                                       &mac);
        if (SGX_SUCCESS != ret)
            break;

        if (0 == consttime_memequal(p_mac, mac, sizeof(mac)))
        {
            ret = SGX_ERROR_MAC_MISMATCH;
            break;
        }

    } while (0);

    return ret;
}

/*
 *  @brief check mr_signer and mr_enclave
 *  @param quote quote data
 *  @param quote_size the length of quote
 *  @param mr_signer_good the mr_signer
 *  @param mr_signer_good_size the length of mr_signer_good
 *  @param mr_enclave_good the mr_enclave
 *  @param mr_enclave_good_size the length of mr_enclave_good
 *  @return SGX_ERROR_INVALID_PARAMETER paramater is incorrect
 *  @return SGX_ERROR_UNEXPECTED mr_signer or mr_enclave is invalid
 */
sgx_status_t enclave_verify_quote_policy(uint8_t *quote, uint32_t quote_size,
                                         const char *mr_signer_good, uint32_t mr_signer_good_size,
                                         const char *mr_enclave_good, uint32_t mr_enclave_good_size)
{
    if (quote == NULL || mr_signer_good == NULL || mr_enclave_good == NULL)
    {
        log_d("quote or mr_signer_good or mr_enclave_good is null");
        return SGX_ERROR_INVALID_PARAMETER;
    }
    string mr_signer_str;
    string mr_enclave_str;
    char mr_signer_temp[3] = {0};
    char mr_enclave_temp[3] = {0};
    sgx_quote3_t *p_sgx_quote = (sgx_quote3_t *)quote;
    for (int i = 0; i < SGX_HASH_SIZE; i++)
    {
        snprintf(mr_signer_temp, sizeof(mr_signer_temp), "%02x", p_sgx_quote->report_body.mr_signer.m[i]);
        snprintf(mr_enclave_temp, sizeof(mr_enclave_temp), "%02x", p_sgx_quote->report_body.mr_enclave.m[i]);
        mr_signer_str += mr_signer_temp;
        mr_enclave_str += mr_enclave_temp;
    }
    if ((mr_signer_str.size() != mr_signer_good_size) ||
        (mr_enclave_str.size() != mr_enclave_good_size))
    {
        log_d("mr_signer_str length is not same with mr_signer_good_size or\
                mr_enclave_str length is not same with mr_enclave_good_size!\n");
        return SGX_ERROR_UNEXPECTED;
    }
    if (strncmp(mr_signer_good, mr_signer_str.c_str(), mr_signer_str.size()) != 0 ||
        strncmp(mr_enclave_good, mr_enclave_str.c_str(), mr_enclave_str.size()) != 0)
    {
        log_d("mr_signer or mr_enclave is invalid!\n");
        return SGX_ERROR_UNEXPECTED;
    }
    return SGX_SUCCESS;
}