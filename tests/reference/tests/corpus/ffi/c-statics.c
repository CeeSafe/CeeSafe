/* The C half of ffi/c-statics.cee: registers CeeSafe's static device, calls through its ops table, and prints what
 * it sees. */
#include <stdio.h>
#include <string.h>

#include "ceesafe_exports.h"

int32_t c_main(void);

static int g_released;

void cs_release_static(struct cs_dev *dev) {
    (void)dev;
    g_released++;
}

int32_t cs_register(struct cs_dev *dev) {
    printf("%s %u %u %zu %d\n", dev->name, (unsigned)dev->mtu, (unsigned)dev->flags, strlen(dev->name),
           dev->priv == NULL && dev->ops->pending == NULL);
    return dev->ops->transmit(dev, 100);
}

int32_t c_main(void) {
    struct cs_dev *lo = cs_loopback();
    int32_t sent = cs_init();
    lo->ops->release(lo);
    printf("%d %llu %llu %d %u %d\n", (int)sent, (unsigned long long)lo->stats.packets,
           (unsigned long long)lo->stats.bytes, g_released, (unsigned)lo->mac[5], cs_hook_set());
    printf("%s %s\n", cs_names()[0]->name, cs_names()[1]->name);
    return 0;
}
