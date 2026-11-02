/* A C library the test builds as a static archive (tools/package_tests.py; ADR 0047). */
#ifndef MYLIB_H
#define MYLIB_H
#include <stdint.h>
#define MYLIB_SCALE 2
int32_t mylib_add(int32_t a, int32_t b);
#endif
