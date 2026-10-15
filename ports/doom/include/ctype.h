/*
 * ctype.h: the character classes of Doom on Rach2, those of the C locale
 * for ASCII. toupper and tolower are in crt0.c, and the tests are inline.
 */
#ifndef _CTYPE_H
#define _CTYPE_H

int toupper(int c);
int tolower(int c);

static inline int isalpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline int isdigit(int c) {
    return c >= '0' && c <= '9';
}

static inline int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

static inline int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static inline int isupper(int c) {
    return c >= 'A' && c <= 'Z';
}

static inline int islower(int c) {
    return c >= 'a' && c <= 'z';
}

static inline int isprint(int c) {
    return c >= 0x20 && c <= 0x7e;
}

static inline int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

#endif /* _CTYPE_H */
