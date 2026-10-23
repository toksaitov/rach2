/* tls.c: a private test authority and TLS server, with no command-line tool. */
#include "api.h"
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <openssl/rand.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#endif
static EVP_PKEY *key(void)
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    EVP_PKEY *result = NULL;
    if (ctx && EVP_PKEY_keygen_init(ctx) > 0 &&
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) > 0)
        EVP_PKEY_keygen(ctx, &result);
    EVP_PKEY_CTX_free(ctx);
    return result;
}
static int extension(X509 *cert, X509 *issuer, int nid, const char *setting)
{
    X509V3_CTX ctx;
    X509_EXTENSION *ext;
    int ok;
    X509V3_set_ctx(&ctx, issuer, cert, NULL, NULL, 0);
    ext = X509V3_EXT_conf_nid(NULL, &ctx, nid, (char *)setting);
    if (!ext)
        return 0;
    ok = X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
    return ok;
}
static X509 *certificate(EVP_PKEY *subject, X509 *issuer, const char *cn)
{
    X509 *cert = X509_new();
    X509_NAME *name = NULL;
    unsigned char serial[8];
    if (!cert || RAND_bytes(serial, sizeof(serial)) != 1)
        goto bad;
    serial[0] &= 0x7f;
    if (!ASN1_STRING_set(X509_get_serialNumber(cert), serial, sizeof(serial)) ||
        !X509_set_version(cert, 2) || !X509_gmtime_adj(X509_getm_notBefore(cert), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(cert), 2 * 86400) || !X509_set_pubkey(cert, subject))
        goto bad;
    name = X509_NAME_new();
    if (!name)
        goto bad;
    if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, (const unsigned char *)cn, -1, -1,
                                    0) ||
        !X509_set_subject_name(cert, name) ||
        !X509_set_issuer_name(cert, issuer ? X509_get_subject_name(issuer) : name))
        goto bad;
    X509_NAME_free(name);
    return cert;
bad:
    X509_NAME_free(name);
    X509_free(cert);
    return NULL;
}
static int write_pem(const char *dir, const char *name, X509 *cert, EVP_PKEY *pkey)
{
    struct buffer path = {0};
    FILE *f;
    int ok;
    putsb(&path, dir);
    putsb(&path, "/");
    putsb(&path, name);
    f = fopen(path.data, "wb");
    if (!f) {
        clear(&path);
        return 0;
    }
    service_mode(path.data);
    ok = cert ? PEM_write_X509(f, cert) : PEM_write_PrivateKey(f, pkey, NULL, NULL, 0, NULL, NULL);
    if (fclose(f))
        ok = 0;
    clear(&path);
    return ok;
}
int make_ca(const char *dir, int n, char **names)
{
    EVP_PKEY *cakey = NULL, *serverkey = NULL;
    X509 *ca = NULL, *server = NULL;
    GENERAL_NAMES *san = NULL;
    int i, ok = 0;
    if (!n || service_mkdir(dir)) {
        log_line("ca: cannot make %s", dir);
        return 2;
    }
    cakey = key();
    serverkey = key();
    if (!cakey || !serverkey)
        goto done;
    ca = certificate(cakey, NULL, "Rach2 fakeapi test CA");
    if (!ca || !extension(ca, ca, NID_basic_constraints, "critical,CA:TRUE") ||
        !extension(ca, ca, NID_key_usage, "critical,keyCertSign,cRLSign") ||
        !extension(ca, ca, NID_subject_key_identifier, "hash") ||
        !X509_sign(ca, cakey, EVP_sha256()))
        goto done;
    server = certificate(serverkey, ca, names[0]);
    san = sk_GENERAL_NAME_new_null();
    if (!server || !san)
        goto done;
    for (i = 0; i < n; i++) {
        GENERAL_NAME *name = GENERAL_NAME_new();
        unsigned char address[16];
        int len;
        if (!name)
            goto done;
        len = inet_pton(AF_INET, names[i], address) == 1    ? 4
              : inet_pton(AF_INET6, names[i], address) == 1 ? 16
                                                            : 0;
        if (len) {
            ASN1_OCTET_STRING *ip = ASN1_OCTET_STRING_new();
            if (!ip || !ASN1_OCTET_STRING_set(ip, address, len)) {
                ASN1_OCTET_STRING_free(ip);
                GENERAL_NAME_free(name);
                goto done;
            }
            GENERAL_NAME_set0_value(name, GEN_IPADD, ip);
        } else {
            ASN1_IA5STRING *dns = ASN1_IA5STRING_new();
            if (!dns || !ASN1_STRING_set(dns, names[i], -1)) {
                ASN1_IA5STRING_free(dns);
                GENERAL_NAME_free(name);
                goto done;
            }
            GENERAL_NAME_set0_value(name, GEN_DNS, dns);
        }
        if (!sk_GENERAL_NAME_push(san, name)) {
            GENERAL_NAME_free(name);
            goto done;
        }
    }
    if (!X509_add1_ext_i2d(server, NID_subject_alt_name, san, 0, 0) ||
        !extension(server, ca, NID_basic_constraints, "critical,CA:FALSE") ||
        !extension(server, ca, NID_key_usage, "critical,digitalSignature") ||
        !extension(server, ca, NID_ext_key_usage, "serverAuth") ||
        !extension(server, ca, NID_subject_key_identifier, "hash") ||
        !extension(server, ca, NID_authority_key_identifier, "keyid") ||
        !X509_sign(server, cakey, EVP_sha256()))
        goto done;
    ok = write_pem(dir, "ca.pem", ca, NULL) && write_pem(dir, "ca.key", NULL, cakey) &&
         write_pem(dir, "server.pem", server, NULL) &&
         write_pem(dir, "server.key", NULL, serverkey);
done:
    GENERAL_NAMES_free(san);
    X509_free(server);
    X509_free(ca);
    EVP_PKEY_free(serverkey);
    EVP_PKEY_free(cakey);
    log_line(ok ? "ca: made %s/ca.pem and a server certificate"
                : "ca: certificate generation failed in %s",
             dir);
    return ok ? 0 : 2;
}
static int alpn(SSL *ssl, const unsigned char **out, unsigned char *len, const unsigned char *in,
                unsigned inlen, void *arg)
{
    unsigned char *selected = NULL;
    (void)ssl;
    (void)arg;
    if (SSL_select_next_proto(&selected, len, (const unsigned char *)"\x08http/1.1", 9, in,
                              inlen) != OPENSSL_NPN_NEGOTIATED)
        return SSL_TLSEXT_ERR_NOACK;
    *out = selected;
    return SSL_TLSEXT_ERR_OK;
}
SSL_CTX *server_context(const char *dir)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    struct buffer path = {0};
    if (!ctx)
        return NULL;
    putsb(&path, dir);
    putsb(&path, "/server.pem");
    if (SSL_CTX_use_certificate_chain_file(ctx, path.data) != 1)
        goto bad;
    clear(&path);
    putsb(&path, dir);
    putsb(&path, "/server.key");
    if (SSL_CTX_use_PrivateKey_file(ctx, path.data, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1)
        goto bad;
    SSL_CTX_set_alpn_select_cb(ctx, alpn, NULL);
    clear(&path);
    return ctx;
bad:
    clear(&path);
    SSL_CTX_free(ctx);
    return NULL;
}
