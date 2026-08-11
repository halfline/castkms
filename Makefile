# SPDX-License-Identifier: GPL-2.0-only

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean install kunit

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		CONFIG_DRM_CASTKMS_KUNIT_TEST=m clean

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install

kunit:
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		CONFIG_DRM_CASTKMS_KUNIT_TEST=m modules
