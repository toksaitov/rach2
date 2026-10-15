/**
 * mkhash_hosttest.c: the crypt of the mkhash password hasher
 * (`krffs/mkhash.krffs.c`).
 *
 * host-deps: vendor/musl/musl-1.2.5/src/crypt/crypt_sha512.c
 *
 * Links the vendored musl SHA-512 crypt as the host tool does and checks it
 * against two vectors of the SHA-crypt specification and one of the
 * project's own, so that a musl update that changes the algorithm fails here
 * rather than as a login on the guest that no longer verifies. Also checks
 * the refusal: a malformed setting gives the `*` failure token or NULL, and
 * never a hash. Exits with the count of failed checks, at most 255.
 */

#include <stdio.h>
#include <string.h>

char *__crypt_sha512(const char *key, const char *setting, char *output);

/**
 * One vector: crypt of `key` under `setting` must give `want`. Returns 1 on
 * a mismatch, which main counts, and 0 on a match.
 */
static int check(const char *key, const char *setting, const char *want)
{
    char output[256];
    char *hash = __crypt_sha512(key, setting, output);

    if (hash == NULL || strcmp(hash, want) != 0) {
        fprintf(stderr, "mkhash_hosttest: crypt(%s, %s) != %s\n", key, setting, want);
        return 1;
    }
    return 0;
}

/** Check the vectors and the malformed setting; the count of failures. */
int main(void)
{
    int failed = 0;

    failed += check("Hello world!", "$6$saltstring",
                    "$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/"
                    "O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1");
    failed += check("Hello world!", "$6$rounds=10000$saltstringsaltstring",
                    "$6$rounds=10000$saltstringsaltst$OW1/O6BYHV6BcXZu8QVeXbDWra3Oeqh0sb"
                    "HbbMCVNSnCM/UrjmM0Dp8vOuZeHBy/YTBmSK6H9qs/y3RnOaw5v.");
    failed += check("rach2", "$6$rach2test",
                    "$6$rach2test$br2Wb86wTtgiPWLN2WQoPqu69kGMNLnIQj3eaZLhG6sNTm9sN6ny"
                    ".2af9tuaB/2HTw86.N3ZZN585IBoPgi0w/");

    {
        char output[256];
        char *hash = __crypt_sha512("key", "$1$notsha", output);

        if (hash != NULL && hash[0] != '*') {
            fprintf(stderr, "mkhash_hosttest: malformed setting minted a hash\n");
            failed += 1;
        }
    }

    return failed > 255 ? 255 : failed;
}
