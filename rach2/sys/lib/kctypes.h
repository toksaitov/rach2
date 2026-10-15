#ifndef _KCTYPES_H_
#define _KCTYPES_H_

/* Case conversion for ASCII letters, without a locale. */

/** Returns `c` upcased if it is a lowercase letter, else `c` unchanged. */
char k_toupper(char c);

/** Returns `c` downcased if it is an uppercase letter, else `c` unchanged. */
char k_tolower(char c);

#endif
