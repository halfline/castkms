# SPDX-License-Identifier: GPL-2.0-only

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all check check-architecture \
	check-shell clean install kunit tools

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		CONFIG_DRM_CASTKMS_KUNIT_TEST=m clean
	$(MAKE) -C tools clean

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install

kunit:
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		CONFIG_DRM_CASTKMS_KUNIT_TEST=m modules

check: check-architecture check-shell
	$(MAKE) -C tools check

check-architecture:
	./scripts/check-architecture.sh

check-shell:
	bash -O nullglob -c 'files=(scripts/*.sh scripts/vm/*.sh tools/pw-castkms/*.sh); \
		bash -n "$${files[@]}"'
	# Guest helpers use dynamic source paths, controlled sysfs names, and
	# caller-owned result redirections around sudo commands.
	bash -O nullglob -c 'files=(scripts/*.sh scripts/vm/*.sh tools/pw-castkms/*.sh); \
		shellcheck --external-sources --source-path=scripts \
		--source-path=scripts/vm \
		--exclude=SC1090,SC2012,SC2024 "$${files[@]}"'

tools:
	$(MAKE) -C tools
