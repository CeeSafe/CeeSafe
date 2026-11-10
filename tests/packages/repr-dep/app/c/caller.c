/* C that uses a dependency's @repr types by their declared names (type-system.md §28.5). */
#include "ceesafe_exports.h"

int32_t caller_total(void);

static int32_t norm(struct Point p) { return p.x + p.y; }

int32_t caller_total(void) {
    struct Point p = { 1, 1 };
    geo_shift(&p, 1);                                     /* (2, 2) */
    struct Rect r = { { 0, 0 }, p };
    Shade dark = app_flip(Shade_Light);
    int32_t total = geo_area(r, dark);                    /* 8 */
    total += app_measure((struct Point){ 2, 5 }, Shade_Light); /* 10 */
    struct Path path = { { { 1, 2 }, { 3, 4 } }, 5 };
    total += geo_path_sum(path);                          /* 15 */
    total += geo_apply(norm);                             /* 2 */
    total += (int32_t)geo_corners;                        /* 4 */
    return total + (int32_t)sizeof(struct Rect);          /* 16 */
}
