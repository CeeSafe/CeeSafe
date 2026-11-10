# SPDX-License-Identifier: GPL-2.0-only
# `make gen` needs ceesafec (on any host: the C is the same); `make` needs only a kernel build tree and its compiler.
KDIR ?= /lib/modules/$(shell uname -r)/build
CEESAFEC ?= ceesafec

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

gen:
	$(CEESAFEC) build --emit c -o gen

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all gen clean
