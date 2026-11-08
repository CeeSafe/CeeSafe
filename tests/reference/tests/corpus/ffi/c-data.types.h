/* ffi/c-data.cee's C side (ADR 0080): the constants a port copies, data CeeSafe defines and C names, data C defines
 * and CeeSafe reads, a char by value, a function named like a struct, a member array of pointers, a byte member C
 * changes atomically, and an unsigned long array -- the shapes CosmoOS's storage floors had. */
#ifndef C_DATA_TYPES_H
#define C_DATA_TYPES_H

#include <stddef.h>
#include <stdint.h>

#define EIO 5
#define DATA_VERSION "data 2.0"
#define PAGE_SHIFT 12
enum node_kind { NODE_FILE = 1, NODE_DIR = 2 };

struct dev {
    uint32_t id;
};
struct stats {
    uint64_t reads;
};
struct pool {
    uint32_t count;
    struct dev *spare[4];
    const char *tags[2];
    uint8_t dumpable;
};
struct ops {
    int32_t (*twice)(int32_t);
    uint32_t flags;
};

extern const struct ops data_ops; /* CeeSafe's */
extern uint32_t g_data_rate;      /* CeeSafe's; C writes it */
extern const char *const data_names[2];
extern uint32_t g_data_counter;

int32_t c_main(void);
int32_t data_mac(char domain, const uint8_t *p);
struct pool *data_pool(void);
void stats(struct stats *out);
void data_loadavg(unsigned long out[3]);

#endif
