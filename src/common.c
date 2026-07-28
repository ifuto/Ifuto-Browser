#include "common.h"
#include <stdio.h>
#include <stdlib.h>

_Noreturn void if_fatal(const char *msg) {
    fprintf(stderr, "ifuto: fatal: %s\n", msg);
    abort();
}
