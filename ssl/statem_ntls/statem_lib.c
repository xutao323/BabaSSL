/*
 * Copyright 2019 The BabaSSL Project Authors. All Rights Reserved.
 */

#include <limits.h>
#include <string.h>
#include <stdio.h>
#include "ssl_local_ntls.h"
#include "statem_local_ntls.h"
#include "internal/cryptlib.h"
#include <openssl/buffer.h>
#include <openssl/objects.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#if (!defined OPENSSL_NO_NTLS) && (!defined OPENSSL_NO_SM2)    \
     && (!defined OPENSSL_NO_SM3) && (!defined OPENSSL_NO_SM4)
static int ssl_add_cert_to_wpacket_ntls(SSL *s, WPACKET *pkt, X509 *x);
/*
 * Map error codes to TLS/SSL alart types.
 */
typedef struct x509err2alert_st {
    int x509err;
    int alert;
} X509ERR2ALERT;

/* Fixed value used in the ServerHello random field to identify an HRR */
const unsigned char hrrrandom_ntls[] = {
    0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02,
    0x1e, 0x65, 0xb8, 0x91, 0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e,
    0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c
};

/*
 * send s->init_buf in records of type 'type' (SSL3_RT_HANDSHAKE or
 * SSL3_RT_CHANGE_CIPHER_SPEC)
 */
int ssl3_do_write_ntls(SSL *s, int type)
{
    int ret;
    size_t written = 0;

    ret = ssl3_write_bytes(s, type, &s->init_buf->data[s->init_off],
                           s->init_num, &written);
    if (ret < 0)
        return -1;
    if (type == SSL3_RT_HANDSHAKE)
        /*
         * should not be done for 'Hello Request's, but in that case we'll
         * ignore the result anyway
         */
        if (!ssl3_finish_mac(s,
                             (unsigned char *)&s->init_buf->data[s->init_off],
                             written))
            return -1;
    if (written == s->init_num) {
        if (s->msg_callback)
            s->msg_callback(1, s->version, type, s->init_buf->data,
                            (size_t)(s->init_off + s->init_num), s,
                            s->msg_callback_arg);
        return 1;
    }
    s->init_off += written;
    s->init_num -= written;
    return 0;
}

int tls_close_construct_packet_ntls(SSL *s, WPACKET *pkt, int htype)
{
    size_t msglen;

    if ((htype != SSL3_MT_CHANGE_CIPHER_SPEC && !WPACKET_close(pkt))
            || !WPACKET_get_length(pkt, &msglen)
            || msglen > INT_MAX)
        return 0;
    s->init_num = (int)msglen;
    s->init_off = 0;

    return 1;
}

int tls_setup_handshake_ntls(SSL *s)
{
    if (!ssl3_init_finished_mac(s)) {
        /* SSLfatal_ntls() already called */
        return 0;
    }

    /* Reset any extension flags */
    memset(s->ext.extflags, 0, sizeof(s->ext.extflags));

    if (s->server) {
        STACK_OF(SSL_CIPHER) *ciphers = SSL_get_ciphers(s);
        int i, ver_min, ver_max, ok = 0;

        /*
         * Sanity check that the maximum version we accept has ciphers
         * enabled. For clients we do this check during construction of the
         * ClientHello.
         */
        if (ssl_get_min_max_version_ntls(s, &ver_min, &ver_max, NULL) != 0) {
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_SETUP_HANDSHAKE_NTLS,
                     ERR_R_INTERNAL_ERROR);
            return 0;
        }
        for (i = 0; i < sk_SSL_CIPHER_num(ciphers); i++) {
            const SSL_CIPHER *c = sk_SSL_CIPHER_value(ciphers, i);

            if (ver_max >= c->min_tls && ver_max <= c->max_tls) {
                ok = 1;
            }
            if (ok)
                break;
        }
        if (!ok) {
            SSLfatal_ntls(s, SSL_AD_HANDSHAKE_FAILURE, SSL_F_TLS_SETUP_HANDSHAKE_NTLS,
                     SSL_R_NO_CIPHERS_AVAILABLE);
            ERR_add_error_data(1, "No ciphers enabled for max supported "
                                  "SSL/TLS version");
            return 0;
        }
        if (SSL_IS_FIRST_HANDSHAKE(s)) {
            /* N.B. s->session_ctx == s->ctx here */
            tsan_counter(&s->session_ctx->stats.sess_accept);
        } else {
            /* N.B. s->ctx may not equal s->session_ctx */
            tsan_counter(&s->ctx->stats.sess_accept_renegotiate);

            s->s3->tmp.cert_request = 0;
        }
    } else {
        if (SSL_IS_FIRST_HANDSHAKE(s))
            tsan_counter(&s->session_ctx->stats.sess_connect);
        else
            tsan_counter(&s->session_ctx->stats.sess_connect_renegotiate);

        /* mark client_random uninitialized */
        memset(s->s3->client_random, 0, sizeof(s->s3->client_random));
        s->hit = 0;

        s->s3->tmp.cert_req = 0;

    }

    return 1;
}

/*
 * Size of the to-be-signed TLS13 data, without the hash size itself:
 * 64 bytes of value 32, 33 context bytes, 1 byte separator
 */
# define TLS13_TBS_START_SIZE            64
# define TLS13_TBS_PREAMBLE_SIZE         (TLS13_TBS_START_SIZE + 33 + 1)

static int get_cert_verify_tbs_data(SSL *s, unsigned char *tls13tbs,
                                    void **hdata, size_t *hdatalen)
{
    {
        size_t retlen;
        long retlen_l;

        retlen = retlen_l = BIO_get_mem_data(s->s3->handshake_buffer, hdata);
        if (retlen_l <= 0) {
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_GET_CERT_VERIFY_TBS_DATA,
                     ERR_R_INTERNAL_ERROR);
            return 0;
        }
        *hdatalen = retlen;
    }

    return 1;
}

MSG_PROCESS_RETURN tls_process_cert_verify_ntls(SSL *s, PACKET *pkt)
{
    EVP_PKEY *pkey = NULL;
    const unsigned char *data;
# ifndef OPENSSL_NO_GOST
    unsigned char *gost_data = NULL;
# endif
    MSG_PROCESS_RETURN ret = MSG_PROCESS_ERROR;
    int j;
    unsigned int len;
    X509 *peer;
    const EVP_MD *md = NULL;
    size_t hdatalen = 0;
    void *hdata;
    unsigned char tls13tbs[TLS13_TBS_PREAMBLE_SIZE + EVP_MAX_MD_SIZE];
    EVP_MD_CTX *mctx = EVP_MD_CTX_new();
    EVP_PKEY_CTX *pctx = NULL;

    if (mctx == NULL) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                 ERR_R_MALLOC_FAILURE);
        goto err;
    }

    peer = s->session->peer;
    pkey = X509_get0_pubkey(peer);
    if (pkey == NULL) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if (ssl_cert_lookup_by_pkey(pkey, NULL) == NULL) {
        SSLfatal_ntls(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                 SSL_R_SIGNATURE_FOR_NON_SIGNING_CERTIFICATE);
        goto err;
    }

    if (SSL_USE_SIGALGS(s)) {
        unsigned int sigalg;

        if (!PACKET_get_net_2(pkt, &sigalg)) {
            SSLfatal_ntls(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                     SSL_R_BAD_PACKET);
            goto err;
        }
        if (tls12_check_peer_sigalg(s, sigalg, pkey) <= 0) {
            /* SSLfatal_ntls() already called */
            goto err;
        }
    } else if (!tls1_set_peer_legacy_sigalg(s, pkey)) {
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                     ERR_R_INTERNAL_ERROR);
            goto err;
    }

    if (!tls1_lookup_md(s->s3->tmp.peer_sigalg, &md)) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }

# ifdef SSL_DEBUG
    if (SSL_USE_SIGALGS(s))
        fprintf(stderr, "USING TLSv1.2 HASH %s\n",
                md == NULL ? "n/a" : EVP_MD_name(md));
# endif

    /* Check for broken implementations of GOST ciphersuites */
    /*
     * If key is GOST and len is exactly 64 or 128, it is signature without
     * length field (CryptoPro implementations at least till TLS 1.2)
     */
# ifndef OPENSSL_NO_GOST
    if (!SSL_USE_SIGALGS(s)
        && ((PACKET_remaining(pkt) == 64
             && (EVP_PKEY_id(pkey) == NID_id_GostR3410_2001
                 || EVP_PKEY_id(pkey) == NID_id_GostR3410_2012_256))
            || (PACKET_remaining(pkt) == 128
                && EVP_PKEY_id(pkey) == NID_id_GostR3410_2012_512))) {
        len = PACKET_remaining(pkt);
    } else
# endif
    if (!PACKET_get_net_2(pkt, &len)) {
        SSLfatal_ntls(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                 SSL_R_LENGTH_MISMATCH);
        goto err;
    }

    j = EVP_PKEY_size(pkey);
    if (((int)len > j) || ((int)PACKET_remaining(pkt) > j)
        || (PACKET_remaining(pkt) == 0)) {
        SSLfatal_ntls(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                 SSL_R_WRONG_SIGNATURE_SIZE);
        goto err;
    }
    if (!PACKET_get_bytes(pkt, &data, len)) {
        SSLfatal_ntls(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                 SSL_R_LENGTH_MISMATCH);
        goto err;
    }

    if (!get_cert_verify_tbs_data(s, tls13tbs, &hdata, &hdatalen)) {
        /* SSLfatal_ntls() already called */
        goto err;
    }

# ifdef SSL_DEBUG
    fprintf(stderr, "Using client verify alg %s\n",
            md == NULL ? "n/a" : EVP_MD_name(md));
# endif

# ifndef OPENSSL_NO_SM2
    if (EVP_PKEY_is_sm2(pkey))  {
        if (!EVP_PKEY_set_alias_type(pkey, EVP_PKEY_SM2)) {
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                     ERR_R_INTERNAL_ERROR);
            goto err;
        }

        if (pkey != NULL) {
            pctx = EVP_PKEY_CTX_new(pkey, NULL);

            if (EVP_PKEY_CTX_set1_id(pctx, HANDSHAKE_SM2_ID, HANDSHAKE_SM2_ID_LEN) != 1) {
                SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                         ERR_R_EVP_LIB);
            }
            EVP_MD_CTX_set_pkey_ctx(mctx, pctx);
        }

        if (!EVP_PKEY_set_alias_type(pkey, EVP_PKEY_EC)) {
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                             ERR_R_INTERNAL_ERROR);
            goto err;
        }
    }
# endif

    if (EVP_DigestVerifyInit(mctx, &pctx, md, NULL, pkey) <= 0) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                 ERR_R_EVP_LIB);
        goto err;
    }
# ifndef OPENSSL_NO_GOST
    {
        int pktype = EVP_PKEY_id(pkey);
        if (pktype == NID_id_GostR3410_2001
            || pktype == NID_id_GostR3410_2012_256
            || pktype == NID_id_GostR3410_2012_512) {
            if ((gost_data = OPENSSL_malloc(len)) == NULL) {
                SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR,
                         SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS, ERR_R_MALLOC_FAILURE);
                goto err;
            }
            BUF_reverse(gost_data, data, len);
            data = gost_data;
        }
    }
# endif

    if (SSL_USE_PSS(s)) {
        if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0
            || EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx,
                                                RSA_PSS_SALTLEN_DIGEST) <= 0) {
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                     ERR_R_EVP_LIB);
            goto err;
        }
    }
    if (s->version == SSL3_VERSION) {
        if (EVP_DigestVerifyUpdate(mctx, hdata, hdatalen) <= 0
                || !EVP_MD_CTX_ctrl(mctx, EVP_CTRL_SSL3_MASTER_SECRET,
                                    (int)s->session->master_key_length,
                                    s->session->master_key)) {
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                     ERR_R_EVP_LIB);
            goto err;
        }
        if (EVP_DigestVerifyFinal(mctx, data, len) <= 0) {
            SSLfatal_ntls(s, SSL_AD_DECRYPT_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                     SSL_R_BAD_SIGNATURE);
            goto err;
        }
    } else {
        j = EVP_DigestVerify(mctx, data, len, hdata, hdatalen);
        if (j <= 0) {
            SSLfatal_ntls(s, SSL_AD_DECRYPT_ERROR, SSL_F_TLS_PROCESS_CERT_VERIFY_NTLS,
                     SSL_R_BAD_SIGNATURE);
            goto err;
        }
    }

    ret = MSG_PROCESS_CONTINUE_READING;
 err:
    BIO_free(s->s3->handshake_buffer);
    s->s3->handshake_buffer = NULL;
    EVP_MD_CTX_free(mctx);
# ifndef OPENSSL_NO_GOST
    OPENSSL_free(gost_data);
# endif
# ifndef OPENSSL_NO_SM2
    if (pkey != NULL && EVP_PKEY_is_sm2(pkey))  {
        /*other sig call EVP_PKEY_CTX_free there may cause segfault */
        EVP_PKEY_CTX_free(pctx);
    }
# endif
    return ret;
}

int tls_construct_finished_ntls(SSL *s, WPACKET *pkt)
{
    size_t finish_md_len;
    const char *sender;
    size_t slen;

    /* This is a real handshake so make sure we clean it up at the end */
    if (!s->server && s->post_handshake_auth != SSL_PHA_REQUESTED)
        s->statem.cleanuphand = 1;

    if (s->server) {
        sender = s->method->ssl3_enc->server_finished_label;
        slen = s->method->ssl3_enc->server_finished_label_len;
    } else {
        sender = s->method->ssl3_enc->client_finished_label;
        slen = s->method->ssl3_enc->client_finished_label_len;
    }

    finish_md_len = s->method->ssl3_enc->final_finish_mac(s,
                                                          sender, slen,
                                                          s->s3->tmp.finish_md);
    if (finish_md_len == 0) {
        /* SSLfatal_ntls() already called */
        return 0;
    }

    s->s3->tmp.finish_md_len = finish_md_len;

    if (!WPACKET_memcpy(pkt, s->s3->tmp.finish_md, finish_md_len)) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_FINISHED_NTLS,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    /*
     * Log the master secret, if logging is enabled. We don't log it for
     * TLSv1.3: there's a different key schedule for that.
     */
    if (!ssl_log_secret(s, MASTER_SECRET_LABEL,
                        s->session->master_key,
                        s->session->master_key_length)) {
        /* SSLfatal_ntls() already called */
        return 0;
    }

    /*
     * Copy the finished so we can use it for renegotiation checks
     */
    if (!ossl_assert(finish_md_len <= EVP_MAX_MD_SIZE)) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_FINISHED_NTLS,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }
    if (!s->server) {
        memcpy(s->s3->previous_client_finished, s->s3->tmp.finish_md,
               finish_md_len);
        s->s3->previous_client_finished_len = finish_md_len;
    } else {
        memcpy(s->s3->previous_server_finished, s->s3->tmp.finish_md,
               finish_md_len);
        s->s3->previous_server_finished_len = finish_md_len;
    }

    return 1;
}

int tls_construct_key_update_ntls(SSL *s, WPACKET *pkt)
{
    if (!WPACKET_put_bytes_u8(pkt, s->key_update)) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_KEY_UPDATE_NTLS,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    s->key_update = SSL_KEY_UPDATE_NONE;
    return 1;
}

MSG_PROCESS_RETURN tls_process_key_update_ntls(SSL *s, PACKET *pkt)
{
    unsigned int updatetype;

    /*
     * A KeyUpdate message signals a key change so the end of the message must
     * be on a record boundary.
     */
    if (RECORD_LAYER_processed_read_pending(&s->rlayer)) {
        SSLfatal_ntls(s, SSL_AD_UNEXPECTED_MESSAGE, SSL_F_TLS_PROCESS_KEY_UPDATE_NTLS,
                 SSL_R_NOT_ON_RECORD_BOUNDARY);
        return MSG_PROCESS_ERROR;
    }

    if (!PACKET_get_1(pkt, &updatetype)
            || PACKET_remaining(pkt) != 0) {
        SSLfatal_ntls(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_KEY_UPDATE_NTLS,
                 SSL_R_BAD_KEY_UPDATE);
        return MSG_PROCESS_ERROR;
    }

    /*
     * There are only two defined key update types. Fail if we get a value we
     * didn't recognise.
     */
    if (updatetype != SSL_KEY_UPDATE_NOT_REQUESTED
            && updatetype != SSL_KEY_UPDATE_REQUESTED) {
        SSLfatal_ntls(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_TLS_PROCESS_KEY_UPDATE_NTLS,
                 SSL_R_BAD_KEY_UPDATE);
        return MSG_PROCESS_ERROR;
    }

    /*
     * If we get a request for us to update our sending keys too then, we need
     * to additionally send a KeyUpdate message. However that message should
     * not also request an update (otherwise we get into an infinite loop).
     */
    if (updatetype == SSL_KEY_UPDATE_REQUESTED)
        s->key_update = SSL_KEY_UPDATE_NOT_REQUESTED;

    if (!tls13_update_key(s, 0)) {
        /* SSLfatal_ntls() already called */
        return MSG_PROCESS_ERROR;
    }

    return MSG_PROCESS_FINISHED_READING;
}

/*
 * ssl3_take_mac_ntls calculates the Finished MAC for the handshakes messages seen
 * to far.
 */
int ssl3_take_mac_ntls(SSL *s)
{
    const char *sender;
    size_t slen;

    if (!s->server) {
        sender = s->method->ssl3_enc->server_finished_label;
        slen = s->method->ssl3_enc->server_finished_label_len;
    } else {
        sender = s->method->ssl3_enc->client_finished_label;
        slen = s->method->ssl3_enc->client_finished_label_len;
    }

    s->s3->tmp.peer_finish_md_len =
        s->method->ssl3_enc->final_finish_mac(s, sender, slen,
                                              s->s3->tmp.peer_finish_md);

    if (s->s3->tmp.peer_finish_md_len == 0) {
        /* SSLfatal_ntls() already called */
        return 0;
    }

    return 1;
}

MSG_PROCESS_RETURN tls_process_change_cipher_spec_ntls(SSL *s, PACKET *pkt)
{
    size_t remain;

    remain = PACKET_remaining(pkt);
    /*
     * 'Change Cipher Spec' is just a single byte, which should already have
     * been consumed by ssl_get_message() so there should be no bytes left,
     */
    if (remain != 0) {
        SSLfatal_ntls(s, SSL_AD_DECODE_ERROR,
                    SSL_F_TLS_PROCESS_CHANGE_CIPHER_SPEC_NTLS,
                    SSL_R_BAD_CHANGE_CIPHER_SPEC);
        return MSG_PROCESS_ERROR;
    }


    /* Check we have a cipher to change to */
    if (s->s3->tmp.new_cipher == NULL) {
        SSLfatal_ntls(s, SSL_AD_UNEXPECTED_MESSAGE,
                 SSL_F_TLS_PROCESS_CHANGE_CIPHER_SPEC_NTLS, SSL_R_CCS_RECEIVED_EARLY);
        return MSG_PROCESS_ERROR;
    }

    s->s3->change_cipher_spec = 1;
    if (!ssl3_do_change_cipher_spec(s)) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_CHANGE_CIPHER_SPEC_NTLS,
                 ERR_R_INTERNAL_ERROR);
        return MSG_PROCESS_ERROR;
    }

    return MSG_PROCESS_CONTINUE_READING;
}

MSG_PROCESS_RETURN tls_process_finished_ntls(SSL *s, PACKET *pkt)
{
    size_t md_len;


    /* This is a real handshake so make sure we clean it up at the end */
    if (s->server) {
        /*
        * To get this far we must have read encrypted data from the client. We
        * no longer tolerate unencrypted alerts. This value is ignored if less
        * than TLSv1.3
        */
        s->statem.enc_read_state = ENC_READ_STATE_VALID;
        if (s->post_handshake_auth != SSL_PHA_REQUESTED)
            s->statem.cleanuphand = 1;
    }

    /* If this occurs, we have missed a message */
    if (!s->s3->change_cipher_spec) {
        SSLfatal_ntls(s, SSL_AD_UNEXPECTED_MESSAGE, SSL_F_TLS_PROCESS_FINISHED_NTLS,
                 SSL_R_GOT_A_FIN_BEFORE_A_CCS);
        return MSG_PROCESS_ERROR;
    }
    s->s3->change_cipher_spec = 0;

    md_len = s->s3->tmp.peer_finish_md_len;

    if (md_len != PACKET_remaining(pkt)) {
        SSLfatal_ntls(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_FINISHED_NTLS,
                 SSL_R_BAD_DIGEST_LENGTH);
        return MSG_PROCESS_ERROR;
    }

    if (CRYPTO_memcmp(PACKET_data(pkt), s->s3->tmp.peer_finish_md,
                      md_len) != 0) {
        SSLfatal_ntls(s, SSL_AD_DECRYPT_ERROR, SSL_F_TLS_PROCESS_FINISHED_NTLS,
                 SSL_R_DIGEST_CHECK_FAILED);
        return MSG_PROCESS_ERROR;
    }

    /*
     * Copy the finished so we can use it for renegotiation checks
     */
    if (!ossl_assert(md_len <= EVP_MAX_MD_SIZE)) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_FINISHED_NTLS,
                 ERR_R_INTERNAL_ERROR);
        return MSG_PROCESS_ERROR;
    }
    if (s->server) {
        memcpy(s->s3->previous_client_finished, s->s3->tmp.peer_finish_md,
               md_len);
        s->s3->previous_client_finished_len = md_len;
    } else {
        memcpy(s->s3->previous_server_finished, s->s3->tmp.peer_finish_md,
               md_len);
        s->s3->previous_server_finished_len = md_len;
    }

    return MSG_PROCESS_FINISHED_READING;
}

int tls_construct_change_cipher_spec_ntls(SSL *s, WPACKET *pkt)
{
    if (!WPACKET_put_bytes_u8(pkt, SSL3_MT_CCS)) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR,
                 SSL_F_TLS_CONSTRUCT_CHANGE_CIPHER_SPEC_NTLS, ERR_R_INTERNAL_ERROR);
        return 0;
    }

    return 1;
}

/* Add a certificate to the WPACKET */
static int ssl_add_cert_to_wpacket_ntls(SSL *s, WPACKET *pkt, X509 *x)
{
    int len;
    unsigned char *outbytes;

    len = i2d_X509(x, NULL);
    if (len < 0) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR,
                      SSL_F_SSL_ADD_CERT_TO_WPACKET_NTLS,
                      ERR_R_BUF_LIB);
        return 0;
    }
    if (!WPACKET_sub_allocate_bytes_u24(pkt, len, &outbytes)
            || i2d_X509(x, &outbytes) != len) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR,
                      SSL_F_SSL_ADD_CERT_TO_WPACKET_NTLS,
                      ERR_R_INTERNAL_ERROR);
        return 0;
    }

    return 1;
}

/* Add certificate chain to provided WPACKET */
static int ssl_add_cert_chain_ntls(SSL *s, WPACKET *pkt,
                                   CERT_PKEY *a_cpk, CERT_PKEY *k_cpk)
{
    int i, chain_count;
    X509 *x;
    STACK_OF(X509) *extra_certs;
    STACK_OF(X509) *chain = NULL;
    X509_STORE *chain_store;

    if (a_cpk == NULL || a_cpk->x509 == NULL
        || k_cpk == NULL || k_cpk->x509 == NULL)
        return 1;

    if (a_cpk->chain != NULL)
        extra_certs = a_cpk->chain;
    else if (k_cpk->chain != NULL)
        extra_certs = k_cpk->chain;
    else
        extra_certs = s->ctx->extra_certs;

    if ((s->mode & SSL_MODE_NO_AUTO_CHAIN) || extra_certs)
        chain_store = NULL;
    else if (s->cert->chain_store)
        chain_store = s->cert->chain_store;
    else
        chain_store = s->ctx->cert_store;

    if (chain_store != NULL) {
        X509_STORE_CTX *xs_ctx = X509_STORE_CTX_new();

        if (xs_ctx == NULL) {
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR,
                          SSL_F_SSL_ADD_CERT_CHAIN_NTLS,
                          ERR_R_MALLOC_FAILURE);
            return 0;
        }

        if (!X509_STORE_CTX_init(xs_ctx, chain_store,
                                 a_cpk->x509, NULL)) {
            X509_STORE_CTX_free(xs_ctx);
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR,
                          SSL_F_SSL_ADD_CERT_CHAIN_NTLS,
                          ERR_R_X509_LIB);
            return 0;
        }
        /*
         * It is valid for the chain not to be complete (because normally we
         * don't include the root cert in the chain). Therefore we deliberately
         * ignore the error return from this call. We're not actually verifying
         * the cert - we're just building as much of the chain as we can
         */
        (void)X509_verify_cert(xs_ctx);
        ERR_clear_error();
        chain = X509_STORE_CTX_get0_chain(xs_ctx);
        i = ssl_security_cert_chain(s, chain, NULL, 0);
        if (i != 1) {
            X509_STORE_CTX_free(xs_ctx);
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR,
                          SSL_F_SSL_ADD_CERT_CHAIN_NTLS, i);
            return 0;
        }

        /* add sign certificate */
        if (!ssl_add_cert_to_wpacket_ntls(s, pkt, a_cpk->x509)) {
            /* SSLfatal_ntls() already called */
            X509_STORE_CTX_free(xs_ctx);
            return 0;
        }

        /* add encryption certificate */
        if (!ssl_add_cert_to_wpacket_ntls(s, pkt, k_cpk->x509)) {
            /* SSLfatal_ntls() already called */
            X509_STORE_CTX_free(xs_ctx);
            return 0;
        }

        chain_count = sk_X509_num(chain);
        for (i = 1; i < chain_count; i++) {
            x = sk_X509_value(chain, i);
            if (!ssl_add_cert_to_wpacket_ntls(s, pkt, x)) {
                /* SSLfatal_ntls() already called */
                X509_STORE_CTX_free(xs_ctx);
                return 0;
            }
        }
        X509_STORE_CTX_free(xs_ctx);
    } else {
        i = ssl_security_cert_chain(s, extra_certs, a_cpk->x509, 0);
        if (i != 1) {
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR,
                          SSL_F_SSL_ADD_CERT_CHAIN_NTLS, i);
            return 0;
        }

        /* add sign certificate */
        if (!ssl_add_cert_to_wpacket_ntls(s, pkt, a_cpk->x509)) {
            /* SSLfatal_ntls() already called */
            return 0;
        }

        /* add encryption certificate */
        if (!ssl_add_cert_to_wpacket_ntls(s, pkt, k_cpk->x509)) {
            /* SSLfatal_ntls() already called */
            return 0;
        }

        /* output the following chain */
        for (i = 0; i < sk_X509_num(extra_certs); i++) {
            x = sk_X509_value(extra_certs, i);
            if (!ssl_add_cert_to_wpacket_ntls(s, pkt, x)) {
                /* SSLfatal_ntls() already called */
                return 0;
            }
        }
    }

    return 1;
}

unsigned long ssl3_output_cert_chain_ntls(SSL *s, WPACKET *pkt,
                                          CERT_PKEY *a_cpk,
                                          CERT_PKEY *k_cpk)
{
    if (!WPACKET_start_sub_packet_u24(pkt)) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR,
                      SSL_F_SSL3_OUTPUT_CERT_CHAIN_NTLS,
                      ERR_R_INTERNAL_ERROR);
        return 0;
    }

    if (!ssl_add_cert_chain_ntls(s, pkt, a_cpk, k_cpk))
        return 0;

    if (!WPACKET_close(pkt)) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR,
                      SSL_F_SSL3_OUTPUT_CERT_CHAIN_NTLS,
                      ERR_R_INTERNAL_ERROR);
        return 0;
    }

    return 1;
}

/*
 * Tidy up after the end of a handshake. In the case of SCTP this may result
 * in NBIO events. If |clearbufs| is set then init_buf and the wbio buffer is
 * freed up as well.
 */
WORK_STATE tls_finish_handshake_ntls(SSL *s, WORK_STATE wst, int clearbufs, int stop)
{
    void (*cb) (const SSL *ssl, int type, int val) = NULL;
    int cleanuphand = s->statem.cleanuphand;

    if (clearbufs) {
# ifndef OPENSSL_NO_SCTP
        if (BIO_dgram_is_sctp(SSL_get_wbio(s))) {
            BUF_MEM_free(s->init_buf);
            s->init_buf = NULL;
        }
# endif
        if (!ssl_free_wbio_buffer(s)) {
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_FINISH_HANDSHAKE_NTLS,
                     ERR_R_INTERNAL_ERROR);
            return WORK_ERROR;
        }
        s->init_num = 0;
    }

    /*
     * Only set if there was a Finished message and this isn't after a TLSv1.3
     * post handshake exchange
     */
    if (cleanuphand) {
        /* skipped if we just sent a HelloRequest */
        s->renegotiate = 0;
        s->new_session = 0;
        s->statem.cleanuphand = 0;
        s->ext.ticket_expected = 0;

        ssl3_cleanup_key_block(s);

        if (s->server) {
            ssl_update_cache(s, SSL_SESS_CACHE_SERVER);

            /* N.B. s->ctx may not equal s->session_ctx */
            tsan_counter(&s->ctx->stats.sess_accept_good);
            s->handshake_func = ossl_statem_accept_ntls;
        } else {
            ssl_update_cache(s, SSL_SESS_CACHE_CLIENT);
            if (s->hit)
                tsan_counter(&s->session_ctx->stats.sess_hit);

            s->handshake_func = ossl_statem_connect_ntls;
            tsan_counter(&s->session_ctx->stats.sess_connect_good);
        }
    }

    if (s->info_callback != NULL)
        cb = s->info_callback;
    else if (s->ctx->info_callback != NULL)
        cb = s->ctx->info_callback;

    /* The callback may expect us to not be in init at handshake done */
    ossl_statem_set_in_init_ntls(s, 0);

    if (cb != NULL) {
        if (cleanuphand
                || SSL_IS_FIRST_HANDSHAKE(s))
            cb(s, SSL_CB_HANDSHAKE_DONE, 1);
    }

    if (!stop) {
        /* If we've got more work to do we go back into init */
        ossl_statem_set_in_init_ntls(s, 1);
        return WORK_FINISHED_CONTINUE;
    }

    return WORK_FINISHED_STOP;
}

int tls_get_message_header_ntls(SSL *s, int *mt)
{
    /* s->init_num < SSL3_HM_HEADER_LENGTH */
    int skip_message, i, recvd_type;
    unsigned char *p;
    size_t l, readbytes;

    p = (unsigned char *)s->init_buf->data;

    do {
        while (s->init_num < SSL3_HM_HEADER_LENGTH) {
            i = s->method->ssl_read_bytes(s, SSL3_RT_HANDSHAKE, &recvd_type,
                                          &p[s->init_num],
                                          SSL3_HM_HEADER_LENGTH - s->init_num,
                                          0, &readbytes);
            if (i <= 0) {
                s->rwstate = SSL_READING;
                return 0;
            }
            if (recvd_type == SSL3_RT_CHANGE_CIPHER_SPEC) {
                /*
                 * A ChangeCipherSpec must be a single byte and may not occur
                 * in the middle of a handshake message.
                 */
                if (s->init_num != 0 || readbytes != 1 || p[0] != SSL3_MT_CCS) {
                    SSLfatal_ntls(s, SSL_AD_UNEXPECTED_MESSAGE,
                             SSL_F_TLS_GET_MESSAGE_HEADER_NTLS,
                             SSL_R_BAD_CHANGE_CIPHER_SPEC);
                    return 0;
                }
                if (s->statem.hand_state == TLS_ST_BEFORE
                        && (s->s3->flags & TLS1_FLAGS_STATELESS) != 0) {
                    /*
                     * We are stateless and we received a CCS. Probably this is
                     * from a client between the first and second ClientHellos.
                     * We should ignore this, but return an error because we do
                     * not return success until we see the second ClientHello
                     * with a valid cookie.
                     */
                    return 0;
                }
                s->s3->tmp.message_type = *mt = SSL3_MT_CHANGE_CIPHER_SPEC;
                s->init_num = readbytes - 1;
                s->init_msg = s->init_buf->data;
                s->s3->tmp.message_size = readbytes;
                return 1;
            } else if (recvd_type != SSL3_RT_HANDSHAKE) {
                SSLfatal_ntls(s, SSL_AD_UNEXPECTED_MESSAGE,
                         SSL_F_TLS_GET_MESSAGE_HEADER_NTLS,
                         SSL_R_CCS_RECEIVED_EARLY);
                return 0;
            }
            s->init_num += readbytes;
        }

        skip_message = 0;
        if (!s->server)
            if (s->statem.hand_state != TLS_ST_OK
                    && p[0] == SSL3_MT_HELLO_REQUEST)
                /*
                 * The server may always send 'Hello Request' messages --
                 * we are doing a handshake anyway now, so ignore them if
                 * their format is correct. Does not count for 'Finished'
                 * MAC.
                 */
                if (p[1] == 0 && p[2] == 0 && p[3] == 0) {
                    s->init_num = 0;
                    skip_message = 1;

                    if (s->msg_callback)
                        s->msg_callback(0, s->version, SSL3_RT_HANDSHAKE,
                                        p, SSL3_HM_HEADER_LENGTH, s,
                                        s->msg_callback_arg);
                }
    } while (skip_message);
    /* s->init_num == SSL3_HM_HEADER_LENGTH */

    *mt = *p;
    s->s3->tmp.message_type = *(p++);

    if (RECORD_LAYER_is_sslv2_record(&s->rlayer)) {
        /*
         * Only happens with SSLv3+ in an SSLv2 backward compatible
         * ClientHello
         *
         * Total message size is the remaining record bytes to read
         * plus the SSL3_HM_HEADER_LENGTH bytes that we already read
         */
        l = RECORD_LAYER_get_rrec_length(&s->rlayer)
            + SSL3_HM_HEADER_LENGTH;
        s->s3->tmp.message_size = l;

        s->init_msg = s->init_buf->data;
        s->init_num = SSL3_HM_HEADER_LENGTH;
    } else {
        n2l3(p, l);
        /* BUF_MEM_grow takes an 'int' parameter */
        if (l > (INT_MAX - SSL3_HM_HEADER_LENGTH)) {
            SSLfatal_ntls(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_TLS_GET_MESSAGE_HEADER_NTLS,
                     SSL_R_EXCESSIVE_MESSAGE_SIZE);
            return 0;
        }
        s->s3->tmp.message_size = l;

        s->init_msg = s->init_buf->data + SSL3_HM_HEADER_LENGTH;
        s->init_num = 0;
    }

    return 1;
}

int tls_get_message_body_ntls(SSL *s, size_t *len)
{
    size_t n, readbytes;
    unsigned char *p;
    int i;

    if (s->s3->tmp.message_type == SSL3_MT_CHANGE_CIPHER_SPEC) {
        /* We've already read everything in */
        *len = (unsigned long)s->init_num;
        return 1;
    }

    p = s->init_msg;
    n = s->s3->tmp.message_size - s->init_num;
    while (n > 0) {
        i = s->method->ssl_read_bytes(s, SSL3_RT_HANDSHAKE, NULL,
                                      &p[s->init_num], n, 0, &readbytes);
        if (i <= 0) {
            s->rwstate = SSL_READING;
            *len = 0;
            return 0;
        }
        s->init_num += readbytes;
        n -= readbytes;
    }

    /*
     * If receiving Finished, record MAC of prior handshake messages for
     * Finished verification.
     */
    if (*(s->init_buf->data) == SSL3_MT_FINISHED && !ssl3_take_mac_ntls(s)) {
        /* SSLfatal_ntls() already called */
        *len = 0;
        return 0;
    }

    /* Feed this message into MAC computation. */
    if (RECORD_LAYER_is_sslv2_record(&s->rlayer)) {
        if (!ssl3_finish_mac(s, (unsigned char *)s->init_buf->data,
                             s->init_num)) {
            /* SSLfatal_ntls() already called */
            *len = 0;
            return 0;
        }
        if (s->msg_callback)
            s->msg_callback(0, SSL2_VERSION, 0, s->init_buf->data,
                            (size_t)s->init_num, s, s->msg_callback_arg);
    } else {
        /*
         * We defer feeding in the HRR until later. We'll do it as part of
         * processing the message
         */
# define SERVER_HELLO_RANDOM_OFFSET  (SSL3_HM_HEADER_LENGTH + 2)
        if (s->s3->tmp.message_type != SSL3_MT_SERVER_HELLO
                || s->init_num < SERVER_HELLO_RANDOM_OFFSET + SSL3_RANDOM_SIZE
                || memcmp(hrrrandom_ntls,
                          s->init_buf->data + SERVER_HELLO_RANDOM_OFFSET,
                          SSL3_RANDOM_SIZE) != 0) {
            if (!ssl3_finish_mac(s, (unsigned char *)s->init_buf->data,
                                 s->init_num + SSL3_HM_HEADER_LENGTH)) {
                /* SSLfatal_ntls() already called */
                *len = 0;
                return 0;
            }
        }
        if (s->msg_callback)
            s->msg_callback(0, s->version, SSL3_RT_HANDSHAKE, s->init_buf->data,
                            (size_t)s->init_num + SSL3_HM_HEADER_LENGTH, s,
                            s->msg_callback_arg);
    }

    *len = s->init_num;
    return 1;
}

static const X509ERR2ALERT x509table[] = {
    {X509_V_ERR_APPLICATION_VERIFICATION, SSL_AD_HANDSHAKE_FAILURE},
    {X509_V_ERR_CA_KEY_TOO_SMALL, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_CA_MD_TOO_WEAK, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_CERT_CHAIN_TOO_LONG, SSL_AD_UNKNOWN_CA},
    {X509_V_ERR_CERT_HAS_EXPIRED, SSL_AD_CERTIFICATE_EXPIRED},
    {X509_V_ERR_CERT_NOT_YET_VALID, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_CERT_REJECTED, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_CERT_REVOKED, SSL_AD_CERTIFICATE_REVOKED},
    {X509_V_ERR_CERT_SIGNATURE_FAILURE, SSL_AD_DECRYPT_ERROR},
    {X509_V_ERR_CERT_UNTRUSTED, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_CRL_HAS_EXPIRED, SSL_AD_CERTIFICATE_EXPIRED},
    {X509_V_ERR_CRL_NOT_YET_VALID, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_CRL_SIGNATURE_FAILURE, SSL_AD_DECRYPT_ERROR},
    {X509_V_ERR_DANE_NO_MATCH, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT, SSL_AD_UNKNOWN_CA},
    {X509_V_ERR_EE_KEY_TOO_SMALL, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_EMAIL_MISMATCH, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_HOSTNAME_MISMATCH, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_INVALID_CA, SSL_AD_UNKNOWN_CA},
    {X509_V_ERR_INVALID_CALL, SSL_AD_INTERNAL_ERROR},
    {X509_V_ERR_INVALID_PURPOSE, SSL_AD_UNSUPPORTED_CERTIFICATE},
    {X509_V_ERR_IP_ADDRESS_MISMATCH, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_OUT_OF_MEM, SSL_AD_INTERNAL_ERROR},
    {X509_V_ERR_PATH_LENGTH_EXCEEDED, SSL_AD_UNKNOWN_CA},
    {X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN, SSL_AD_UNKNOWN_CA},
    {X509_V_ERR_STORE_LOOKUP, SSL_AD_INTERNAL_ERROR},
    {X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE, SSL_AD_BAD_CERTIFICATE},
    {X509_V_ERR_UNABLE_TO_GET_CRL, SSL_AD_UNKNOWN_CA},
    {X509_V_ERR_UNABLE_TO_GET_CRL_ISSUER, SSL_AD_UNKNOWN_CA},
    {X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT, SSL_AD_UNKNOWN_CA},
    {X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY, SSL_AD_UNKNOWN_CA},
    {X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE, SSL_AD_UNKNOWN_CA},
    {X509_V_ERR_UNSPECIFIED, SSL_AD_INTERNAL_ERROR},

    /* Last entry; return this if we don't find the value above. */
    {X509_V_OK, SSL_AD_CERTIFICATE_UNKNOWN}
};

int ssl_x509err2alert_ntls(int x509err)
{
    const X509ERR2ALERT *tp;

    for (tp = x509table; tp->x509err != X509_V_OK; ++tp)
        if (tp->x509err == x509err)
            break;
    return tp->alert;
}

int ssl_allow_compression_ntls(SSL *s)
{
    if (s->options & SSL_OP_NO_COMPRESSION)
        return 0;
    return ssl_security(s, SSL_SECOP_COMPRESSION, 0, 0, NULL);
}

static int version_cmp(const SSL *s, int a, int b)
{
    if (a == b)
        return 0;
    else
        return a < b ? -1 : 1;
}

typedef struct {
    int version;
    const SSL_METHOD *(*cmeth) (void);
    const SSL_METHOD *(*smeth) (void);
} version_info;

# if TLS_MAX_VERSION != TLS1_3_VERSION
#  error Code needs update for TLS_method() support beyond TLS1_3_VERSION.
# endif

/* Must be in order high to low */
static const version_info tls_version_table[] = {
# ifndef OPENSSL_NO_TLS1_3
    {TLS1_3_VERSION, tlsv1_3_client_method, tlsv1_3_server_method},
# else
    {TLS1_3_VERSION, NULL, NULL},
# endif
# ifndef OPENSSL_NO_TLS1_2
    {TLS1_2_VERSION, tlsv1_2_client_method, tlsv1_2_server_method},
# else
    {TLS1_2_VERSION, NULL, NULL},
# endif
# ifndef OPENSSL_NO_TLS1_1
    {TLS1_1_VERSION, tlsv1_1_client_method, tlsv1_1_server_method},
# else
    {TLS1_1_VERSION, NULL, NULL},
# endif
# ifndef OPENSSL_NO_TLS1
    {TLS1_VERSION, tlsv1_client_method, tlsv1_server_method},
# else
    {TLS1_VERSION, NULL, NULL},
# endif
    /*
     * NTLS is not a standard version, so we don't set
     * {NTLS_VERSION, NULL, NULL},
     */
    {NTLS_VERSION, ntls_client_method, ntls_server_method},
# ifndef OPENSSL_NO_SSL3
    {SSL3_VERSION, sslv3_client_method, sslv3_server_method},
# else
    {SSL3_VERSION, NULL, NULL},
# endif
    {0, NULL, NULL},
};





/*
 * ssl_method_error - Check whether an SSL_METHOD is enabled.
 *
 * @s: The SSL handle for the candidate method
 * @method: the intended method.
 *
 * Returns 0 on success, or an SSL error reason on failure.
 */
static int ssl_method_error(const SSL *s, const SSL_METHOD *method)
{
    int version = method->version;

    if ((s->min_proto_version != 0 &&
         version_cmp(s, version, s->min_proto_version) < 0) ||
        ssl_security(s, SSL_SECOP_VERSION, 0, version, NULL) == 0)
        return SSL_R_VERSION_TOO_LOW;

    if (s->max_proto_version != 0 &&
        version_cmp(s, version, s->max_proto_version) > 0)
        return SSL_R_VERSION_TOO_HIGH;

    if ((s->options & method->mask) != 0)
        return SSL_R_UNSUPPORTED_PROTOCOL;
    if ((method->flags & SSL_METHOD_NO_SUITEB) != 0 && tls1_suiteb(s))
        return SSL_R_AT_LEAST_TLS_1_2_NEEDED_IN_SUITEB_MODE;

    return 0;
}

/*
 * ssl_version_supported_ntls - Check that the specified `version` is supported by
 * `SSL *` instance
 *
 * @s: The SSL handle for the candidate method
 * @version: Protocol version to test against
 *
 * Returns 1 when supported, otherwise 0
 */
int ssl_version_supported_ntls(const SSL *s, int version, const SSL_METHOD **meth)
{
    const version_info *vent;
    const version_info *table;

    switch (s->method->version) {
    default:
        /* Version should match method version for non-ANY method */
        return version_cmp(s, version, s->version) == 0;
    case TLS_ANY_VERSION:
        table = tls_version_table;
        break;
    }

    for (vent = table;
         vent->version != 0 && version_cmp(s, version, vent->version) <= 0;
         ++vent) {
        if (vent->cmeth != NULL
                && version_cmp(s, version, vent->version) == 0
                && ssl_method_error(s, vent->cmeth()) == 0
                && (!s->server
                    || version != TLS1_3_VERSION)) {
            if (meth != NULL)
                *meth = vent->cmeth();
            return 1;
        }
    }
    return 0;
}

/*
 * ssl_check_version_downgrade_ntls - In response to RFC7507 SCSV version
 * fallback indication from a client check whether we're using the highest
 * supported protocol version.
 *
 * @s server SSL handle.
 *
 * Returns 1 when using the highest enabled version, 0 otherwise.
 */
int ssl_check_version_downgrade_ntls(SSL *s)
{
    const version_info *vent;
    const version_info *table;

    /*
     * Check that the current protocol is the highest enabled version
     * (according to s->ctx->method, as version negotiation may have changed
     * s->method).
     */
    if (s->version == s->ctx->method->version)
        return 1;

    /*
     * Apparently we're using a version-flexible SSL_METHOD (not at its
     * highest protocol version).
     */
    if (s->ctx->method->version == TLS_method()->version)
        table = tls_version_table;
    else {
        /* Unexpected state; fail closed. */
        return 0;
    }

    for (vent = table; vent->version != 0; ++vent) {
        if (vent->smeth != NULL && ssl_method_error(s, vent->smeth()) == 0)
            return s->version == vent->version;
    }
    return 0;
}

/*
 * ssl_set_version_bound_ntls - set an upper or lower bound on the supported (D)TLS
 * protocols, provided the initial (D)TLS method is version-flexible.  This
 * function sanity-checks the proposed value and makes sure the method is
 * version-flexible, then sets the limit if all is well.
 *
 * @method_version: The version of the current SSL_METHOD.
 * @version: the intended limit.
 * @bound: pointer to limit to be updated.
 *
 * Returns 1 on success, 0 on failure.
 */
int ssl_set_version_bound_ntls(int method_version, int version, int *bound)
{
    if (version == 0) {
        *bound = version;
        return 1;
    }

    /*-
     * Restrict TLS methods to TLS protocol versions.
     *
     * Note that for both lower-bounds we use explicit versions, not
     * (D)TLS_MIN_VERSION.  This is because we don't want to break user
     * configurations.  If the MIN (supported) version ever rises, the user's
     * "floor" remains valid even if no longer available.  We don't expect the
     * MAX ceiling to ever get lower, so making that variable makes sense.
     */
    switch (method_version) {
    default:
        /*
         * XXX For fixed version methods, should we always fail and not set any
         * bounds, always succeed and not set any bounds, or set the bounds and
         * arrange to fail later if they are not met?  At present fixed-version
         * methods are not subject to controls that disable individual protocol
         * versions.
         */
        if (version == NTLS_VERSION)
            return 1;
        return 0;

    case TLS_ANY_VERSION:
        if ((version < SSL3_VERSION || version > TLS_MAX_VERSION)
            && (version != NTLS_VERSION && version != MIN_VERSION_WITH_NTLS))
            return 0;
        break;
    }

    *bound = version;
    return 1;
}

static void check_for_downgrade(SSL *s, int vers, DOWNGRADE *dgrd)
{
    if (vers == TLS1_2_VERSION
            && ssl_version_supported_ntls(s, TLS1_3_VERSION, NULL)) {
        *dgrd = DOWNGRADE_TO_1_2;
    } else if (vers < TLS1_2_VERSION
               /*
                * We need to ensure that a server that disables TLSv1.2
                * (creating a hole between TLSv1.3 and TLSv1.1) can still
                * complete handshakes with clients that support TLSv1.2 and
                * below. Therefore we do not enable the sentinel if TLSv1.3 is
                * enabled and TLSv1.2 is not.
                */
            && ssl_version_supported_ntls(s, TLS1_2_VERSION, NULL)) {
        *dgrd = DOWNGRADE_TO_1_1;
    } else {
        *dgrd = DOWNGRADE_NONE;
    }
}

/*
 * ssl_choose_server_version_ntls - Choose server (D)TLS version.  Called when the
 * client HELLO is received to select the final server protocol version and
 * the version specific method.
 *
 * @s: server SSL handle.
 *
 * Returns 0 on success or an SSL error reason number on failure.
 */
int ssl_choose_server_version_ntls(SSL *s, CLIENTHELLO_MSG *hello, DOWNGRADE *dgrd)
{
    /*-
     * With version-flexible methods we have an initial state with:
     *
     *   s->method->version == (D)TLS_ANY_VERSION,
     *   s->version == (D)TLS_MAX_VERSION.
     *
     * So we detect version-flexible methods via the method version, not the
     * handle version.
     */
    int server_version = s->method->version;
    int client_version = hello->legacy_version;
    const version_info *vent;
    const version_info *table;
    int disabled = 0;
    RAW_EXTENSION *suppversions;

    s->client_version = client_version;

    switch (server_version) {
    default:
        if (version_cmp(s, client_version, s->version) < 0)
            return SSL_R_WRONG_SSL_VERSION;
        *dgrd = DOWNGRADE_NONE;
        /*
            * If this SSL handle is not from a version flexible method we don't
            * (and never did) check min/max FIPS or Suite B constraints.  Hope
            * that's OK.  It is up to the caller to not choose fixed protocol
            * versions they don't want.  If not, then easy to fix, just return
            * ssl_method_error(s, s->method)
            */
        return 0;
        /*
         * Fall through if we are TLSv1.3 already (this means we must be after
         * a HelloRetryRequest
         */
        /* fall thru */
    case TLS_ANY_VERSION:
        table = tls_version_table;
        break;
    }

    suppversions = &hello->pre_proc_exts[TLSEXT_IDX_supported_versions];

    /* If we did an HRR then supported versions is mandatory */
    if (!suppversions->present && s->hello_retry_request != SSL_HRR_NONE)
        return SSL_R_UNSUPPORTED_PROTOCOL;

    if (suppversions->present) {
        unsigned int candidate_vers = 0;
        unsigned int best_vers = 0;
        const SSL_METHOD *best_method = NULL;
        PACKET versionslist;

        suppversions->parsed = 1;

        if (!PACKET_as_length_prefixed_1(&suppversions->data, &versionslist)) {
            /* Trailing or invalid data? */
            return SSL_R_LENGTH_MISMATCH;
        }

        /*
         * The TLSv1.3 spec says the client MUST set this to TLS1_2_VERSION.
         * The spec only requires servers to check that it isn't SSLv3:
         * "Any endpoint receiving a Hello message with
         * ClientHello.legacy_version or ServerHello.legacy_version set to
         * 0x0300 MUST abort the handshake with a "protocol_version" alert."
         * We are slightly stricter and require that it isn't SSLv3 or lower.
         * We tolerate TLSv1 and TLSv1.1.
         */
        if (client_version <= SSL3_VERSION)
            return SSL_R_BAD_LEGACY_VERSION;

        while (PACKET_get_net_2(&versionslist, &candidate_vers)) {
            if (version_cmp(s, candidate_vers, best_vers) <= 0)
                continue;
            if (ssl_version_supported_ntls(s, candidate_vers, &best_method))
                best_vers = candidate_vers;
        }
        if (PACKET_remaining(&versionslist) != 0) {
            /* Trailing data? */
            return SSL_R_LENGTH_MISMATCH;
        }

        if (best_vers > 0) {
            if (s->hello_retry_request != SSL_HRR_NONE) {
                /*
                 * This is after a HelloRetryRequest so we better check that we
                 * negotiated TLSv1.3
                 */
                if (best_vers != TLS1_3_VERSION)
                    return SSL_R_UNSUPPORTED_PROTOCOL;
                return 0;
            }
            check_for_downgrade(s, best_vers, dgrd);
            s->version = best_vers;
            s->method = best_method;
            return 0;
        }
        return SSL_R_UNSUPPORTED_PROTOCOL;
    }

    /*
     * If the supported versions extension isn't present, then the highest
     * version we can negotiate is TLSv1.2
     */
    if (version_cmp(s, client_version, TLS1_3_VERSION) >= 0)
        client_version = TLS1_2_VERSION;

    /*
     * No supported versions extension, so we just use the version supplied in
     * the ClientHello.
     */
    for (vent = table; vent->version != 0; ++vent) {
        const SSL_METHOD *method;

        if (vent->smeth == NULL ||
            version_cmp(s, client_version, vent->version) < 0)
            continue;
        method = vent->smeth();
        if (ssl_method_error(s, method) == 0) {
            check_for_downgrade(s, vent->version, dgrd);
            s->version = vent->version;
            s->method = method;
            return 0;
        }
        disabled = 1;
    }
    return disabled ? SSL_R_UNSUPPORTED_PROTOCOL : SSL_R_VERSION_TOO_LOW;
}

/*
 * ssl_choose_client_version_ntls - Choose client (D)TLS version.  Called when the
 * server HELLO is received to select the final client protocol version and
 * the version specific method.
 *
 * @s: client SSL handle.
 * @version: The proposed version from the server's HELLO.
 * @extensions: The extensions received
 *
 * Returns 1 on success or 0 on error.
 */
int ssl_choose_client_version_ntls(SSL *s, int version, RAW_EXTENSION *extensions)
{
    const version_info *vent;
    const version_info *table;
    int ret, ver_min, ver_max, real_max, origv;

    origv = s->version;
    s->version = version;

    /* This will overwrite s->version if the extension is present */
    if (!tls_parse_extension_ntls(s, TLSEXT_IDX_supported_versions,
                             SSL_EXT_TLS1_2_SERVER_HELLO
                             | SSL_EXT_TLS1_3_SERVER_HELLO, extensions,
                             NULL, 0)) {
        s->version = origv;
        return 0;
    }

    if (s->hello_retry_request != SSL_HRR_NONE
            && s->version != TLS1_3_VERSION) {
        s->version = origv;
        SSLfatal_ntls(s, SSL_AD_PROTOCOL_VERSION, SSL_F_SSL_CHOOSE_CLIENT_VERSION_NTLS,
                 SSL_R_WRONG_SSL_VERSION);
        return 0;
    }

    switch (s->method->version) {
    default:
        if (s->version != s->method->version) {
            s->version = origv;
            SSLfatal_ntls(s, SSL_AD_PROTOCOL_VERSION,
                     SSL_F_SSL_CHOOSE_CLIENT_VERSION_NTLS,
                     SSL_R_WRONG_SSL_VERSION);
            return 0;
        }
        /*
         * If this SSL handle is not from a version flexible method we don't
         * (and never did) check min/max, FIPS or Suite B constraints.  Hope
         * that's OK.  It is up to the caller to not choose fixed protocol
         * versions they don't want.  If not, then easy to fix, just return
         * ssl_method_error(s, s->method)
         */
        return 1;
    case TLS_ANY_VERSION:
        table = tls_version_table;
        break;
    }

    ret = ssl_get_min_max_version_ntls(s, &ver_min, &ver_max, &real_max);
    if (ret != 0) {
        s->version = origv;
        SSLfatal_ntls(s, SSL_AD_PROTOCOL_VERSION,
                 SSL_F_SSL_CHOOSE_CLIENT_VERSION_NTLS, ret);
        return 0;
    }
    if (s->version < ver_min) {
        s->version = origv;
        SSLfatal_ntls(s, SSL_AD_PROTOCOL_VERSION,
                 SSL_F_SSL_CHOOSE_CLIENT_VERSION_NTLS, SSL_R_UNSUPPORTED_PROTOCOL);
        return 0;
    } else if (s->version > ver_max) {
        s->version = origv;
        SSLfatal_ntls(s, SSL_AD_PROTOCOL_VERSION,
                 SSL_F_SSL_CHOOSE_CLIENT_VERSION_NTLS, SSL_R_UNSUPPORTED_PROTOCOL);
        return 0;
    }

    if ((s->mode & SSL_MODE_SEND_FALLBACK_SCSV) == 0)
        real_max = ver_max;

    /* Check for downgrades */
    if (s->version == TLS1_2_VERSION && real_max > s->version) {
        if (memcmp(tls12downgrade,
                   s->s3->server_random + SSL3_RANDOM_SIZE
                                        - sizeof(tls12downgrade),
                   sizeof(tls12downgrade)) == 0) {
            s->version = origv;
            SSLfatal_ntls(s, SSL_AD_ILLEGAL_PARAMETER,
                     SSL_F_SSL_CHOOSE_CLIENT_VERSION_NTLS,
                     SSL_R_INAPPROPRIATE_FALLBACK);
            return 0;
        }
    } else if (s->version < TLS1_2_VERSION
               && real_max > s->version) {
        if (memcmp(tls11downgrade,
                   s->s3->server_random + SSL3_RANDOM_SIZE
                                        - sizeof(tls11downgrade),
                   sizeof(tls11downgrade)) == 0) {
            s->version = origv;
            SSLfatal_ntls(s, SSL_AD_ILLEGAL_PARAMETER,
                     SSL_F_SSL_CHOOSE_CLIENT_VERSION_NTLS,
                     SSL_R_INAPPROPRIATE_FALLBACK);
            return 0;
        }
    }

    for (vent = table; vent->version != 0; ++vent) {
        if (vent->cmeth == NULL || s->version != vent->version)
            continue;

        s->method = vent->cmeth();
        return 1;
    }

    s->version = origv;
    SSLfatal_ntls(s, SSL_AD_PROTOCOL_VERSION, SSL_F_SSL_CHOOSE_CLIENT_VERSION_NTLS,
             SSL_R_UNSUPPORTED_PROTOCOL);
    return 0;
}

/*
 * ssl_get_min_max_version_ntls - get minimum and maximum protocol version
 * @s: The SSL connection
 * @min_version: The minimum supported version
 * @max_version: The maximum supported version
 * @real_max:    The highest version below the lowest compile time version hole
 *               where that hole lies above at least one run-time enabled
 *               protocol.
 *
 * Work out what version we should be using for the initial ClientHello if the
 * version is initially (D)TLS_ANY_VERSION.  We apply any explicit SSL_OP_NO_xxx
 * options, the MinProtocol and MaxProtocol configuration commands, any Suite B
 * constraints and any floor imposed by the security level here,
 * so we don't advertise the wrong protocol version to only reject the outcome later.
 *
 * Computing the right floor matters.  If, e.g., TLS 1.0 and 1.2 are enabled,
 * TLS 1.1 is disabled, but the security level, Suite-B  and/or MinProtocol
 * only allow TLS 1.2, we want to advertise TLS1.2, *not* TLS1.
 *
 * Returns 0 on success or an SSL error reason number on failure.  On failure
 * min_version and max_version will also be set to 0.
 */
int ssl_get_min_max_version_ntls(const SSL *s, int *min_version, int *max_version,
                            int *real_max)
{
    int version, tmp_real_max;
    int hole;
    const SSL_METHOD *single = NULL;
    const SSL_METHOD *method;
    const version_info *table;
    const version_info *vent;

    switch (s->method->version) {
    default:
        /*
         * If this SSL handle is not from a version flexible method we don't
         * (and never did) check min/max FIPS or Suite B constraints.  Hope
         * that's OK.  It is up to the caller to not choose fixed protocol
         * versions they don't want.  If not, then easy to fix, just return
         * ssl_method_error(s, s->method)
         */
        *min_version = *max_version = s->version;
        /*
         * Providing a real_max only makes sense where we're using a version
         * flexible method.
         */
        if (!ossl_assert(real_max == NULL))
            return ERR_R_INTERNAL_ERROR;
        return 0;
    case TLS_ANY_VERSION:
        table = tls_version_table;
        break;
    }

    /*
     * SSL_OP_NO_X disables all protocols above X *if* there are some protocols
     * below X enabled. This is required in order to maintain the "version
     * capability" vector contiguous. Any versions with a NULL client method
     * (protocol version client is disabled at compile-time) is also a "hole".
     *
     * Our initial state is hole == 1, version == 0.  That is, versions above
     * the first version in the method table are disabled (a "hole" above
     * the valid protocol entries) and we don't have a selected version yet.
     *
     * Whenever "hole == 1", and we hit an enabled method, its version becomes
     * the selected version, and the method becomes a candidate "single"
     * method.  We're no longer in a hole, so "hole" becomes 0.
     *
     * If "hole == 0" and we hit an enabled method, then "single" is cleared,
     * as we support a contiguous range of at least two methods.  If we hit
     * a disabled method, then hole becomes true again, but nothing else
     * changes yet, because all the remaining methods may be disabled too.
     * If we again hit an enabled method after the new hole, it becomes
     * selected, as we start from scratch.
     */
    *min_version = version = 0;
    hole = 1;
    if (real_max != NULL)
        *real_max = 0;
    tmp_real_max = 0;
    for (vent = table; vent->version != 0; ++vent) {
        /*
         * A table entry with a NULL client method is still a hole in the
         * "version capability" vector.
         */
        if (vent->cmeth == NULL) {
            hole = 1;
            tmp_real_max = 0;
            continue;
        }
        method = vent->cmeth();

        if (hole == 1 && tmp_real_max == 0)
            tmp_real_max = vent->version;

        if (ssl_method_error(s, method) != 0) {
            hole = 1;
        } else if (!hole) {
            single = NULL;
            *min_version = method->version;
        } else {
            if (real_max != NULL && tmp_real_max != 0)
                *real_max = tmp_real_max;
            version = (single = method)->version;
            *min_version = version;
            hole = 0;
        }
    }

    *max_version = version;

    /* Fail if everything is disabled */
    if (version == 0)
        return SSL_R_NO_PROTOCOLS_AVAILABLE;

    return 0;
}

/*
 * ssl_set_client_hello_version_ntls - Work out what version we should be using for
 * the initial ClientHello.legacy_version field.
 *
 * @s: client SSL handle.
 *
 * Returns 0 on success or an SSL error reason number on failure.
 */
int ssl_set_client_hello_version_ntls(SSL *s)
{
    int ver_min, ver_max, ret;

    /*
     * In a renegotiation we always send the same client_version that we sent
     * last time, regardless of which version we eventually negotiated.
     */
    if (!SSL_IS_FIRST_HANDSHAKE(s))
        return 0;

    ret = ssl_get_min_max_version_ntls(s, &ver_min, &ver_max, NULL);

    if (ret != 0)
        return ret;

    s->version = ver_max;

    /* TLS1.3 always uses TLS1.2 in the legacy_version field */
    if (ver_max > TLS1_2_VERSION)
        ver_max = TLS1_2_VERSION;

    s->client_version = ver_max;
    return 0;
}

/*
 * Checks a list of |groups| to determine if the |group_id| is in it. If it is
 * and |checkallow| is 1 then additionally check if the group is allowed to be
 * used. Returns 1 if the group is in the list (and allowed if |checkallow| is
 * 1) or 0 otherwise.
 */
# ifndef OPENSSL_NO_EC
int check_in_list_ntls(SSL *s, uint16_t group_id, const uint16_t *groups,
                  size_t num_groups, int checkallow)
{
    size_t i;

    if (groups == NULL || num_groups == 0)
        return 0;

    for (i = 0; i < num_groups; i++) {
        uint16_t group = groups[i];

        if (group_id == group
                && (!checkallow
                    || tls_curve_allowed(s, group, SSL_SECOP_CURVE_CHECK))) {
            return 1;
        }
    }

    return 0;
}
# endif

/* Replace ClientHello1 in the transcript hash with a synthetic message */
int create_synthetic_message_hash_ntls(SSL *s, const unsigned char *hashval,
                                  size_t hashlen, const unsigned char *hrr,
                                  size_t hrrlen)
{
    unsigned char hashvaltmp[EVP_MAX_MD_SIZE];
    unsigned char msghdr[SSL3_HM_HEADER_LENGTH];

    memset(msghdr, 0, sizeof(msghdr));

    if (hashval == NULL) {
        hashval = hashvaltmp;
        hashlen = 0;
        /* Get the hash of the initial ClientHello */
        if (!ssl3_digest_cached_records(s, 0)
                || !ssl_handshake_hash(s, hashvaltmp, sizeof(hashvaltmp),
                                       &hashlen)) {
            /* SSLfatal_ntls() already called */
            return 0;
        }
    }

    /* Reinitialise the transcript hash */
    if (!ssl3_init_finished_mac(s)) {
        /* SSLfatal_ntls() already called */
        return 0;
    }

    /* Inject the synthetic message_hash message */
    msghdr[0] = SSL3_MT_MESSAGE_HASH;
    msghdr[SSL3_HM_HEADER_LENGTH - 1] = (unsigned char)hashlen;
    if (!ssl3_finish_mac(s, msghdr, SSL3_HM_HEADER_LENGTH)
            || !ssl3_finish_mac(s, hashval, hashlen)) {
        /* SSLfatal_ntls() already called */
        return 0;
    }

    /*
     * Now re-inject the HRR and current message if appropriate (we just deleted
     * it when we reinitialised the transcript hash above). Only necessary after
     * receiving a ClientHello2 with a cookie.
     */
    if (hrr != NULL
            && (!ssl3_finish_mac(s, hrr, hrrlen)
                || !ssl3_finish_mac(s, (unsigned char *)s->init_buf->data,
                                    s->s3->tmp.message_size
                                    + SSL3_HM_HEADER_LENGTH))) {
        /* SSLfatal_ntls() already called */
        return 0;
    }

    return 1;
}

static int ca_dn_cmp(const X509_NAME *const *a, const X509_NAME *const *b)
{
    return X509_NAME_cmp(*a, *b);
}

int parse_ca_names_ntls(SSL *s, PACKET *pkt)
{
    STACK_OF(X509_NAME) *ca_sk = sk_X509_NAME_new(ca_dn_cmp);
    X509_NAME *xn = NULL;
    PACKET cadns;

    if (ca_sk == NULL) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_PARSE_CA_NAMES_NTLS,
                 ERR_R_MALLOC_FAILURE);
        goto err;
    }
    /* get the CA RDNs */
    if (!PACKET_get_length_prefixed_2(pkt, &cadns)) {
        SSLfatal_ntls(s, SSL_AD_DECODE_ERROR,SSL_F_PARSE_CA_NAMES_NTLS,
                 SSL_R_LENGTH_MISMATCH);
        goto err;
    }

    while (PACKET_remaining(&cadns)) {
        const unsigned char *namestart, *namebytes;
        unsigned int name_len;

        if (!PACKET_get_net_2(&cadns, &name_len)
            || !PACKET_get_bytes(&cadns, &namebytes, name_len)) {
            SSLfatal_ntls(s, SSL_AD_DECODE_ERROR, SSL_F_PARSE_CA_NAMES_NTLS,
                     SSL_R_LENGTH_MISMATCH);
            goto err;
        }

        namestart = namebytes;
        if ((xn = d2i_X509_NAME(NULL, &namebytes, name_len)) == NULL) {
            SSLfatal_ntls(s, SSL_AD_DECODE_ERROR, SSL_F_PARSE_CA_NAMES_NTLS,
                     ERR_R_ASN1_LIB);
            goto err;
        }
        if (namebytes != (namestart + name_len)) {
            SSLfatal_ntls(s, SSL_AD_DECODE_ERROR, SSL_F_PARSE_CA_NAMES_NTLS,
                     SSL_R_CA_DN_LENGTH_MISMATCH);
            goto err;
        }

        if (!sk_X509_NAME_push(ca_sk, xn)) {
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_PARSE_CA_NAMES_NTLS,
                     ERR_R_MALLOC_FAILURE);
            goto err;
        }
        xn = NULL;
    }

    sk_X509_NAME_pop_free(s->s3->tmp.peer_ca_names, X509_NAME_free);
    s->s3->tmp.peer_ca_names = ca_sk;

    return 1;

 err:
    sk_X509_NAME_pop_free(ca_sk, X509_NAME_free);
    X509_NAME_free(xn);
    return 0;
}

const STACK_OF(X509_NAME) *get_ca_names_ntls(SSL *s)
{
    const STACK_OF(X509_NAME) *ca_sk = NULL;;

    if (s->server) {
        ca_sk = SSL_get_client_CA_list(s);
        if (ca_sk != NULL && sk_X509_NAME_num(ca_sk) == 0)
            ca_sk = NULL;
    }

    if (ca_sk == NULL)
        ca_sk = SSL_get0_CA_list(s);

    return ca_sk;
}

int construct_ca_names_ntls(SSL *s, const STACK_OF(X509_NAME) *ca_sk, WPACKET *pkt)
{
    /* Start sub-packet for client CA list */
    if (!WPACKET_start_sub_packet_u16(pkt)) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_CONSTRUCT_CA_NAMES_NTLS,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    if (ca_sk != NULL) {
        int i;

        for (i = 0; i < sk_X509_NAME_num(ca_sk); i++) {
            unsigned char *namebytes;
            X509_NAME *name = sk_X509_NAME_value(ca_sk, i);
            int namelen;

            if (name == NULL
                    || (namelen = i2d_X509_NAME(name, NULL)) < 0
                    || !WPACKET_sub_allocate_bytes_u16(pkt, namelen,
                                                       &namebytes)
                    || i2d_X509_NAME(name, &namebytes) != namelen) {
                SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_CONSTRUCT_CA_NAMES_NTLS,
                         ERR_R_INTERNAL_ERROR);
                return 0;
            }
        }
    }

    if (!WPACKET_close(pkt)) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_CONSTRUCT_CA_NAMES_NTLS,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    return 1;
}

/* Create a buffer containing data to be signed for server key exchange */
size_t construct_key_exchange_tbs_ntls(SSL *s, unsigned char **ptbs,
                                  const void *param, size_t paramlen)
{
    size_t tbslen = 2 * SSL3_RANDOM_SIZE + paramlen;
    unsigned char *tbs = OPENSSL_malloc(tbslen);

    if (tbs == NULL) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR, SSL_F_CONSTRUCT_KEY_EXCHANGE_TBS_NTLS,
                 ERR_R_MALLOC_FAILURE);
        return 0;
    }
    memcpy(tbs, s->s3->client_random, SSL3_RANDOM_SIZE);
    memcpy(tbs + SSL3_RANDOM_SIZE, s->s3->server_random, SSL3_RANDOM_SIZE);

    memcpy(tbs + SSL3_RANDOM_SIZE * 2, param, paramlen);

    *ptbs = tbs;
    return tbslen;
}

/*
 * Saves the current handshake digest for Post-Handshake Auth,
 * Done after ClientFinished is processed, done exactly once
 */
int tls13_save_handshake_digest_for_pha_ntls(SSL *s)
{
    if (s->pha_dgst == NULL) {
        if (!ssl3_digest_cached_records(s, 1))
            /* SSLfatal_ntls() already called */
            return 0;

        s->pha_dgst = EVP_MD_CTX_new();
        if (s->pha_dgst == NULL) {
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR,
                     SSL_F_TLS13_SAVE_HANDSHAKE_DIGEST_FOR_PHA_NTLS,
                     ERR_R_INTERNAL_ERROR);
            return 0;
        }
        if (!EVP_MD_CTX_copy_ex(s->pha_dgst,
                                s->s3->handshake_dgst)) {
            SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR,
                     SSL_F_TLS13_SAVE_HANDSHAKE_DIGEST_FOR_PHA_NTLS,
                     ERR_R_INTERNAL_ERROR);
            return 0;
        }
    }
    return 1;
}

/*
 * Restores the Post-Handshake Auth handshake digest
 * Done just before sending/processing the Cert Request
 */
int tls13_restore_handshake_digest_for_pha_ntls(SSL *s)
{
    if (s->pha_dgst == NULL) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR,
                 SSL_F_TLS13_RESTORE_HANDSHAKE_DIGEST_FOR_PHA_NTLS,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }
    if (!EVP_MD_CTX_copy_ex(s->s3->handshake_dgst,
                            s->pha_dgst)) {
        SSLfatal_ntls(s, SSL_AD_INTERNAL_ERROR,
                 SSL_F_TLS13_RESTORE_HANDSHAKE_DIGEST_FOR_PHA_NTLS,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }
    return 1;
}
#endif
