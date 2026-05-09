#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Opaque handle to a TLS server context (cert/key/RNG — shared across connections) */
typedef void *tls_server_ctx_t;

/* Opaque handle to a single TLS connection */
typedef void *tls_conn_t;

/*
 * Generate a self-signed ECDSA P-256 certificate and private key in DER format.
 * On success, *cert_der and *key_der are malloc'd buffers; caller must free().
 * Returns false on any error.
 */
bool tls_generate_selfsigned(uint8_t **cert_der, size_t *cert_len,
                              uint8_t **key_der,  size_t *key_len);

/*
 * Create a TLS server context from DER-encoded cert and private key.
 * Takes ownership of cert_der and key_der (malloc'd by tls_generate_selfsigned);
 * the caller must NOT free them — this function always frees them.
 * Returns NULL on error.
 */
tls_server_ctx_t tls_server_ctx_create(uint8_t *cert_der, size_t cert_len,
                                        uint8_t *key_der,  size_t key_len);

/* Free a TLS server context. */
void tls_server_ctx_free(tls_server_ctx_t ctx);

/*
 * Perform a TLS server handshake on an already-accepted blocking TCP socket.
 * Returns an opaque connection handle on success, or NULL on failure.
 * The caller retains ownership of fd; it must be closed separately after
 * tls_conn_close().
 */
tls_conn_t tls_server_accept_fd(tls_server_ctx_t ctx, int fd);

/* Read decrypted bytes from a TLS connection.  Returns byte count or -1. */
ssize_t tls_conn_read(tls_conn_t conn, void *buf, size_t len);

/* Write bytes over a TLS connection.  Returns byte count or -1. */
ssize_t tls_conn_write(tls_conn_t conn, const void *buf, size_t len);

/* Send close-notify and free the TLS connection state.  Does NOT close fd. */
void tls_conn_close(tls_conn_t conn);
