# OpenSSL

OpenSSL supplies the cryptography and TLS of the musl userland's Wi-Fi tools: wpa_supplicant and hostapd ([vendor/hostap](../hostap/Readme.md)) link its `libcrypto` and `libssl` statically for WPA3's SAE, the EAP methods of WPA2-Enterprise and every cipher and hash of WPA and WPA2. No image stages the libraries themselves, and the musl system has no `openssl` program; the GNU root has its own OpenSSL ([vendor/gnu](../gnu/Readme.md)).

## Sources and license

The source is OpenSSL 4.0.2, the release the GNU layer pins in [`vendor/gnu/sources.txt`](../gnu/sources.txt), whose digest was recorded after its signature was checked. The tarball is 55 MB, too large to keep in Git, so [`build_openssl.sh`](build_openssl.sh) gets it through [`vendor/gnu/fetch.sh`](../gnu/fetch.sh), which downloads it into `vendor/gnu/downloads/` once and checks it, and checks it again against its own pin, SHA-256 `736b467530f916737b7031310ccb21d8218c6229e61e8e160cd1d3458cd543a8`. A build of the musl userland on a new copy therefore needs the network once, or that file copied in.

OpenSSL is under the Apache License 2.0. The script stages `LICENSE.txt` beside the libraries, and every image that stages a program linked with them stages that notice as `/etc/openssl.LICENSE`.

## Local changes

There are no source patches. The configure line builds static libraries for the `linux-x86` or `linux-x86_64` target through the musl wrapper, without threads, shared objects, position-independent code, the loadable modules or the `openssl` program. The legacy provider, which has the MD4, DES and RC4 that MSCHAPv2 and WPA's TKIP need, is built into `libcrypto`, since there are no modules to load it from.

A provider's table of algorithms names every algorithm it has, so a static program links all of them. The configure line leaves out what hostap never calls, which takes a little over half a megabyte from each program: QUIC, DTLS, kernel TLS and the AF_ALG engine; CMS, CMP, certificate transparency, time stamps, SRP and RFC 3779; the SM, GOST, IDEA, SEED, Camellia, ARIA, Blowfish, CAST, RC2, RC5, MDC2 and Whirlpool algorithms, SipHash, scrypt, Argon2, SIV, OCB and the integrity-only ciphers; ML-DSA, SLH-DSA and LMS; binary curves; compression; and the console prompt for a key's passphrase. The assembly implementations stay, among them the constant-time P-256 that SAE uses.

`libcrypto` keeps the compiler's command line for `OpenSSL_version()`, so the wrapper is found on the path by its bare name and no absolute path of the copy is built in.

## Building

```
make openssl
make openssl ARCH=x86_64
```

The build of wpa_supplicant and hostapd makes it first. Each architecture extracts under `build/vendor-work/openssl/` or `build/x86_64/vendor-work/openssl/`, where `configure.log`, `build.log` and `install.log` are kept, and stages `include/`, `lib/libcrypto.a`, `lib/libssl.a` and `LICENSE.txt` under `build/vendor/openssl/` or `build/x86_64/vendor/openssl/`. Configure needs perl, which every build host has. The stamp's inputs are the script, which holds the pin, the musl wrapper, the architecture table and musl's stamp.

## Updating

When the GNU layer moves to another release, change `VERSION` and `TARBALL_SHA256` in `build_openssl.sh` to its, read the release's notes for a removed interface that hostap uses, and build wpa_supplicant and hostapd for both architectures. Until then this build stops with a word that the pins differ.

## Limits

OpenSSL 4.0 deprecates `X509_cmp_current_time()`, which hostap 2.12 still calls; the compiler warns, and the function works. Nothing else in hostap 2.12 needed a change for 4.0, so the GNU root's wpa_supplicant can link the GNU root's OpenSSL 4.0.2 as it is.

## Tests

wpa_supplicant and hostapd exercise the libraries: [vendor/hostap](../hostap/Readme.md#tests) says how.
