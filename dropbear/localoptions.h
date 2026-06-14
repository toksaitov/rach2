/* Rach2 dropbear feature selection (Phase 23). A compact, self-contained modern
 * set that fits the bundled libtomcrypt and Rach2's entropy/pty stack. */
#define DROPBEAR_CURVE25519 1
#define DROPBEAR_ECDH 1
#define DROPBEAR_CHACHA20POLY1305 1
#define DROPBEAR_AES128 1
#define DROPBEAR_AES256 1
#define DROPBEAR_ENABLE_CTR_MODE 1
#define DROPBEAR_HMAC_SHA256 1
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_SVR_PASSWORD_AUTH 1
#define DROPBEAR_ED25519 1
#define DO_MOTD 0
