/* wsclient.c: the guest's verified TLS and masked WebSocket exchange.
 * Usage: wsclient HOST[:PORT] PATH CAFILE MESSAGE... */
#include <openssl/ssl.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
static const char *stage = "arguments";

static int failed(const char *why)
{
    fprintf(stderr, "wsclient: %s: %s\n", stage, why);
    ERR_print_errors_fp(stderr);
    return 0;
}

static int exact(SSL *ssl, void *data, size_t n, int sending, const char *part)
{
    unsigned char *p = data;
    while (n) {
        int got;
        ERR_clear_error();
        errno = 0;
        got = sending ? SSL_write(ssl, p, (int)n) : SSL_read(ssl, p, (int)n);
        if (got <= 0) {
            int saved_errno = errno;
            int error = SSL_get_error(ssl, got);
            fprintf(stderr,
                    "wsclient: %s: %s: %s returned %d, remaining %zu, SSL_get_error %d, "
                    "errno %d (%s)\n",
                    stage, part, sending ? "SSL_write" : "SSL_read", got, n, error,
                    saved_errno, strerror(saved_errno));
            ERR_print_errors_fp(stderr);
            return 0;
        }
        p += got;
        n -= (size_t)got;
    }
    return 1;
}
static int send_frame(SSL *ssl, unsigned opcode, const void *data, size_t n)
{
    unsigned char head[8], mask[4], *payload;
    size_t i, len = 2;
    int ok;
    if (n > 65535)
        return failed("outgoing payload exceeds 65535 bytes");
    if (RAND_bytes(mask, 4) != 1)
        return failed("cannot generate the frame mask");
    head[0] = 0x80 | opcode;
    if (n < 126)
        head[1] = 0x80 | (unsigned char)n;
    else {
        head[1] = 0xfe;
        head[2] = (unsigned char)(n >> 8);
        head[3] = (unsigned char)n;
        len = 4;
    }
    memcpy(head + len, mask, 4);
    len += 4;
    payload = malloc(n ? n : 1);
    if (!payload)
        return failed("cannot allocate the outgoing payload");
    for (i = 0; i < n; i++)
        payload[i] = ((const unsigned char *)data)[i] ^ mask[i % 4];
    ok = exact(ssl, head, len, 1, "frame header") &&
         exact(ssl, payload, n, 1, "frame payload");
    free(payload);
    return ok;
}
static unsigned char *receive_frame(SSL *ssl, unsigned *opcode, size_t *size)
{
    unsigned char head[2], ext[8], *payload;
    uint64_t n;
    unsigned i;
    if (!exact(ssl, head, 2, 0, "frame header"))
        return NULL;
    if ((head[1] & 0x80) || !(head[0] & 0x80) || (head[0] & 0x70)) {
        fprintf(stderr, "wsclient: %s: invalid server frame flags %02x %02x\n", stage,
                (unsigned)head[0], (unsigned)head[1]);
        return NULL;
    }
    *opcode = head[0] & 15;
    n = head[1] & 127;
    if (n == 126) {
        if (!exact(ssl, ext, 2, 0, "16-bit frame length"))
            return NULL;
        n = (ext[0] << 8) | ext[1];
    } else if (n == 127) {
        if (!exact(ssl, ext, 8, 0, "64-bit frame length"))
            return NULL;
        n = 0;
        for (i = 0; i < 8; i++)
            n = (n << 8) | ext[i];
    }
    if (n > 64U * 1024U * 1024U) {
        failed("incoming frame exceeds 64 MiB");
        return NULL;
    }
    payload = malloc((size_t)n + 1);
    if (!payload) {
        failed("cannot allocate the incoming payload");
        return NULL;
    }
    if (!exact(ssl, payload, (size_t)n, 0, "frame payload")) {
        free(payload);
        return NULL;
    }
    payload[n] = 0;
    *size = (size_t)n;
    return payload;
}
static int read_line(SSL *ssl, char *line, size_t size)
{
    size_t n = 0;
    char c;
    while (exact(ssl, &c, 1, 0, "HTTP line")) {
        if (c == '\n') {
            if (n && line[n - 1] == '\r')
                n--;
            line[n] = 0;
            return 1;
        }
        if (n + 1 == size)
            return failed("HTTP line exceeds its buffer");
        line[n++] = c;
    }
    return 0;
}
static int handshake(SSL *ssl, const char *host, const char *path)
{
    char request[4096], line[4096], accept[100] = "", expected[32];
    unsigned char random[16], key[32], digest[SHA_DIGEST_LENGTH];
    stage = "upgrade key";
    if (RAND_bytes(random, sizeof(random)) != 1)
        return failed("cannot generate the upgrade key");
    EVP_EncodeBlock(key, random, sizeof(random));
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: "
             "Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
             path, host, key);
    stage = "upgrade request";
    if (!exact(ssl, request, strlen(request), 1, "HTTP request"))
        return 0;
    stage = "upgrade status";
    if (!read_line(ssl, line, sizeof(line)))
        return 0;
    if (strncmp(line, "HTTP/1.1 101 ", 13)) {
        fprintf(stderr, "wsclient: %s: expected HTTP/1.1 101, received %.200s\n", stage, line);
        return 0;
    }
    stage = "upgrade headers";
    while (read_line(ssl, line, sizeof(line)) && *line) {
        char *colon = strchr(line, ':');
        if (!colon)
            return failed("header has no colon");
        *colon++ = 0;
        while (*colon == ' ' || *colon == '\t')
            colon++;
        if (!strcasecmp(line, "Sec-WebSocket-Accept"))
            snprintf(accept, sizeof(accept), "%s", colon);
    }
    snprintf(request, sizeof(request), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    if (!SHA1((unsigned char *)request, strlen(request), digest))
        return failed("cannot compute the upgrade accept digest");
    EVP_EncodeBlock((unsigned char *)expected, digest, sizeof(digest));
    if (strcmp(accept, expected))
        return failed("Sec-WebSocket-Accept does not match the upgrade key");
    return 1;
}
static int exchange(SSL *ssl, int argc, char **argv)
{
    unsigned char *data = NULL;
    unsigned opcode;
    size_t size;
    int i, status = 1;
    stage = "ping send";
    if (!send_frame(ssl, 9, "keepalive", 9))
        goto done;
    stage = "pong receive";
    data = receive_frame(ssl, &opcode, &size);
    if (!data)
        goto done;
    if (opcode != 10 || size != 9 || memcmp(data, "keepalive", 9)) {
        size_t at;
        fprintf(stderr, "wsclient: %s: expected opcode 10 and keepalive, got opcode %u, %zu bytes\n",
                stage, opcode, size);
        fprintf(stderr, "wsclient: %s: pong prefix ", stage);
        for (at = 0; at < size && at < 8; at++)
            fprintf(stderr, "%02x", (unsigned)data[at]);
        fputc('\n', stderr);
        goto done;
    }
    puts("pong keepalive");
    free(data);
    data = NULL;
    for (i = 4; i < argc; i++) {
        stage = "text send";
        if (!send_frame(ssl, 1, argv[i], strlen(argv[i])))
            goto done;
        stage = "text receive";
        data = receive_frame(ssl, &opcode, &size);
        if (!data)
            goto done;
        if (opcode != 1) {
            fprintf(stderr, "wsclient: %s: expected opcode 1, got %u\n", stage, opcode);
            goto done;
        }
        puts((char *)data);
        free(data);
        data = NULL;
    }
    stage = "close receive";
    data = receive_frame(ssl, &opcode, &size);
    if (!data)
        goto done;
    if (opcode != 8 || size < 2) {
        fprintf(stderr, "wsclient: %s: expected close code, got opcode %u, %zu bytes\n", stage,
                opcode, size);
        goto done;
    }
    stage = "close send";
    if (!send_frame(ssl, 8, data, 2))
        goto done;
    printf("close %u\n", ((unsigned)data[0] << 8) | data[1]);
    status = 0;
done:
    free(data);
    return status == 0;
}

static int connected(BIO *connection, int negotiating)
{
    int result, saved_errno;
    stage = negotiating ? "TLS handshake" : "TLS connect";
    ERR_clear_error();
    errno = 0;
    result = negotiating ? BIO_do_handshake(connection) : BIO_do_connect(connection);
    if (result > 0)
        return 1;
    saved_errno = errno;
    fprintf(stderr, "wsclient: %s: BIO returned %d, retry %d, errno %d (%s)\n", stage, result,
            BIO_should_retry(connection) != 0, saved_errno, strerror(saved_errno));
    ERR_print_errors_fp(stderr);
    return 0;
}

int main(int argc, char **argv)
{
    SSL_CTX *context = NULL;
    BIO *connection = NULL;
    SSL *ssl = NULL;
    char host[512], address[550], *port;
    int status = 1;
    if (argc < 4 || strlen(argv[1]) >= sizeof(host)) {
        failed("expected HOST[:PORT] PATH CAFILE MESSAGE...");
        goto done;
    }
    strcpy(host, argv[1]);
    port = strchr(host, ':');
    if (port)
        *port++ = 0;
    snprintf(address, sizeof(address), "%s:%s", host, port ? port : "443");
    stage = "TLS context and CA";
    context = SSL_CTX_new(TLS_client_method());
    if (!context || !SSL_CTX_load_verify_locations(context, argv[3], NULL)) {
        failed("cannot initialize the TLS context or read the authority");
        goto done;
    }
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
    connection = BIO_new_ssl_connect(context);
    if (!connection) {
        failed("cannot allocate the TLS connection");
        goto done;
    }
    stage = "TLS hostname";
    BIO_get_ssl(connection, &ssl);
    if (!ssl || !SSL_set_tlsext_host_name(ssl, host) ||
        !X509_VERIFY_PARAM_set1_host(SSL_get0_param(ssl), host, 0)) {
        failed("cannot set the verified server name");
        goto done;
    }
    BIO_set_conn_hostname(connection, address);
    if (!connected(connection, 0) || !connected(connection, 1))
        goto done;
    if (SSL_get_verify_result(ssl) != X509_V_OK) {
        failed(X509_verify_cert_error_string(SSL_get_verify_result(ssl)));
        goto done;
    }
    if (handshake(ssl, host, argv[2]) && exchange(ssl, argc, argv))
        status = 0;

done:
    if (status)
        fputs("wsclient: TLS or WebSocket exchange failed\n", stderr);
    BIO_free_all(connection);
    SSL_CTX_free(context);
    return status;
}
