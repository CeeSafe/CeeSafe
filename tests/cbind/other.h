/* A second header, which fixture.h includes: its types are another binding's (tools/cbind_check.py). */
#ifndef CBIND_OTHER_H
#define CBIND_OTHER_H

struct other_thing {
    int a;
};
typedef int (*other_cb)(void *arg);
enum other_mode { OTHER_A, OTHER_B };

int other_get(const struct other_thing *t);

#endif
