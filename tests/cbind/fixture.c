/* What fixture.h and other.h declare, for the program tools/cbind_check.py builds over their bindings. */
#include "fixture.h"

int fx_count = 3;
const struct fx_point fx_origin = {1, 2};
const char *const fx_names[4] = {"a", "b", "c", "d"};
const char *fx_current = "now";

int fx_add(int a, int b) { return a + b; }
struct fx_point fx_make(int x, int y) { return (struct fx_point){x, y}; }
int fx_log(const char *fmt, ...) { return fmt[0]; }
int type(int in) { return in + 1; }
enum fx_color fx_pick(struct fx_box *box, fx_callback cb, const char *const argv[]) {
    (void)cb;
    (void)argv;
    return box->color;
}
void fx_visit(struct fx_node *n, other_cb cb, enum other_mode mode) {
    (void)n;
    (void)cb;
    (void)mode;
}
void fx_vprint(const char *fmt, __builtin_va_list ap) {
    (void)fmt;
    (void)ap;
}
int other_get(const struct other_thing *t) { return t->a; }
