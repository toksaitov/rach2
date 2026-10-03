#ifndef _PANIC_H_
#define _PANIC_H_

#include "display/textcon.h"
#include "kio.h"
#include "lib/kdef.h"

void k_panic(const char *str) K_NORETURN;

#endif
