/* The C types ffi/c-statics.cee imports (ADR 0042): a device and its ops table, as a kernel's drivers declare them. */
#ifndef C_STATICS_TYPES_H
#define C_STATICS_TYPES_H

#include <stdint.h>

struct cs_dev;

struct cs_ops {
    int32_t (*transmit)(struct cs_dev *dev, int32_t length);
    void (*release)(struct cs_dev *dev);
    uint32_t (*pending)(struct cs_dev *dev); /* optional: CeeSafe's table leaves it NULL */
};

struct cs_stats {
    uint64_t packets;
    uint64_t bytes;
};

struct cs_dev {
    char name[8];
    uint32_t mtu;
    uint32_t flags;
    const struct cs_ops *ops;
    void *priv;              /* not declared in CeeSafe: zero */
    struct cs_stats stats;
    uint8_t mac[6];
};

void cs_release_static(struct cs_dev *dev);     /* in c-statics.c */
int32_t cs_register(struct cs_dev *dev);        /* in c-statics.c: calls through the device's ops */

#endif
