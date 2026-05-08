#include "include/badgevms/tls_server.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

/* Per-connection TLS state */
typedef struct {
    mbedtls_ssl_context ssl;
    int                 fd;
} tls_conn_impl_t;

/* Shared server context: SSL config + cert/key + RNG */
typedef struct {
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         cert;
    mbedtls_pk_context       key;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context  entropy;
} tls_server_ctx_impl_t;

/* BIO callbacks: bio_send/bio_recv are called by mbedTLS to do the actual I/O */

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd  = *(int *)ctx;
    int ret = (int)write(fd, buf, len);
    if (ret < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
    return ret;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd  = *(int *)ctx;
    int ret = (int)read(fd, buf, len);
    if (ret < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    if (ret == 0) return MBEDTLS_ERR_NET_CONN_RESET;
    return ret;
}

bool tls_generate_selfsigned(uint8_t **cert_der_out, size_t *cert_len_out,
                              uint8_t **key_der_out,  size_t *key_len_out) {
    bool                     ok = false;
    mbedtls_pk_context       pk;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509write_cert   crt;

    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_x509write_crt_init(&crt);

    static const unsigned char pers[] = "badgevms_ota_tls_keygen";
    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               pers, sizeof(pers) - 1) != 0)
        goto out;

    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0)
        goto out;

    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk),
                             mbedtls_ctr_drbg_random, &ctr_drbg) != 0)
        goto out;

    /* mbedtls_pk_write_key_der writes from the END of the buffer; returns length */
    uint8_t key_buf[256];
    int     key_der_len = mbedtls_pk_write_key_der(&pk, key_buf, sizeof(key_buf));
    if (key_der_len <= 0) goto out;

    /* Build a self-signed certificate */
    unsigned char serial[1] = {1};
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
    if (mbedtls_x509write_crt_set_subject_name(&crt, "CN=badge.why2025.org") != 0) goto out;
    if (mbedtls_x509write_crt_set_issuer_name(&crt, "CN=badge.why2025.org") != 0) goto out;
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    if (mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0) goto out;
    if (mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20350101000000") != 0)
        goto out;
    if (mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) != 0) goto out;

    /* mbedtls_x509write_crt_der also writes from the END of the buffer */
    uint8_t cert_buf[1024];
    int     cert_der_len = mbedtls_x509write_crt_der(&crt, cert_buf, sizeof(cert_buf),
                                                      mbedtls_ctr_drbg_random, &ctr_drbg);
    if (cert_der_len <= 0) goto out;

    *key_der_out  = malloc((size_t)key_der_len);
    *cert_der_out = malloc((size_t)cert_der_len);
    if (!*key_der_out || !*cert_der_out) {
        free(*key_der_out);  *key_der_out  = NULL;
        free(*cert_der_out); *cert_der_out = NULL;
        goto out;
    }

    memcpy(*key_der_out,  key_buf  + sizeof(key_buf)  - (size_t)key_der_len,  (size_t)key_der_len);
    memcpy(*cert_der_out, cert_buf + sizeof(cert_buf) - (size_t)cert_der_len, (size_t)cert_der_len);
    *key_len_out  = (size_t)key_der_len;
    *cert_len_out = (size_t)cert_der_len;
    ok = true;

out:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return ok;
}

tls_server_ctx_t tls_server_ctx_create(const uint8_t *cert_der, size_t cert_len,
                                        const uint8_t *key_der,  size_t key_len) {
    tls_server_ctx_impl_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    mbedtls_ssl_config_init(&s->conf);
    mbedtls_x509_crt_init(&s->cert);
    mbedtls_pk_init(&s->key);
    mbedtls_ctr_drbg_init(&s->ctr_drbg);
    mbedtls_entropy_init(&s->entropy);

    static const unsigned char pers[] = "badgevms_ota_srv";
    if (mbedtls_ctr_drbg_seed(&s->ctr_drbg, mbedtls_entropy_func, &s->entropy,
                               pers, sizeof(pers) - 1) != 0)
        goto fail;

    if (mbedtls_x509_crt_parse_der(&s->cert, cert_der, cert_len) != 0) goto fail;

    if (mbedtls_pk_parse_key(&s->key, key_der, key_len, NULL, 0,
                              mbedtls_ctr_drbg_random, &s->ctr_drbg) != 0)
        goto fail;

    if (mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_SERVER,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        goto fail;

    mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, &s->ctr_drbg);
    mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_NONE);

    if (mbedtls_ssl_conf_own_cert(&s->conf, &s->cert, &s->key) != 0) goto fail;

    return (tls_server_ctx_t)s;

fail:
    tls_server_ctx_free((tls_server_ctx_t)s);
    return NULL;
}

void tls_server_ctx_free(tls_server_ctx_t ctx) {
    tls_server_ctx_impl_t *s = (tls_server_ctx_impl_t *)ctx;
    if (!s) return;
    mbedtls_ssl_config_free(&s->conf);
    mbedtls_x509_crt_free(&s->cert);
    mbedtls_pk_free(&s->key);
    mbedtls_ctr_drbg_free(&s->ctr_drbg);
    mbedtls_entropy_free(&s->entropy);
    free(s);
}

tls_conn_t tls_server_accept_fd(tls_server_ctx_t ctx, int fd) {
    tls_server_ctx_impl_t *s    = (tls_server_ctx_impl_t *)ctx;
    tls_conn_impl_t       *conn = calloc(1, sizeof(*conn));
    if (!conn) return NULL;

    conn->fd = fd;
    mbedtls_ssl_init(&conn->ssl);

    if (mbedtls_ssl_setup(&conn->ssl, &s->conf) != 0) goto fail;

    mbedtls_ssl_set_bio(&conn->ssl, &conn->fd, bio_send, bio_recv, NULL);

    int ret;
    while ((ret = mbedtls_ssl_handshake(&conn->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            goto fail;
        }
    }

    return (tls_conn_t)conn;

fail:
    mbedtls_ssl_free(&conn->ssl);
    free(conn);
    return NULL;
}

ssize_t tls_conn_read(tls_conn_t conn, void *buf, size_t len) {
    tls_conn_impl_t *c   = (tls_conn_impl_t *)conn;
    int              ret = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, len);
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    if (ret < 0) return -1;
    return (ssize_t)ret;
}

ssize_t tls_conn_write(tls_conn_t conn, const void *buf, size_t len) {
    tls_conn_impl_t   *c    = (tls_conn_impl_t *)conn;
    const uint8_t     *ptr  = (const uint8_t *)buf;
    size_t             sent = 0;
    while (sent < len) {
        int ret = mbedtls_ssl_write(&c->ssl, ptr + sent, len - sent);
        if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret < 0) return sent > 0 ? (ssize_t)sent : -1;
        sent += (size_t)ret;
    }
    return (ssize_t)sent;
}

void tls_conn_close(tls_conn_t conn) {
    tls_conn_impl_t *c = (tls_conn_impl_t *)conn;
    if (!c) return;
    mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_ssl_free(&c->ssl);
    free(c);
}
