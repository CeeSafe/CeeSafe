/* wrap's own C file: it needs mylib.h from wrap's include-dirs and mylib from its links (ADR 0047). */
#include "mylib.h"
int32_t wrap_add(int32_t a, int32_t b) { return mylib_add(a, b) * MYLIB_SCALE; }
