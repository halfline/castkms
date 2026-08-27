# SPDX-License-Identifier: GPL-2.0-only

KDIR ?= /lib/modules/$(shell uname -r)/build
CASTKMS_BUILD_AUDIO ?= y

CASTKMS_KBUILD_OPTIONS := CASTKMS_BUILD_AUDIO=$(CASTKMS_BUILD_AUDIO)

.PHONY: all build-matrix check check-architecture check-ioctls \
	check-shell clean install kunit module-clean tools

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(CASTKMS_KBUILD_OPTIONS) modules

module-clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		$(CASTKMS_KBUILD_OPTIONS) CONFIG_DRM_CASTKMS_KUNIT_TEST=m clean

clean: module-clean
	$(MAKE) -C tools clean

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		$(CASTKMS_KBUILD_OPTIONS) modules_install

kunit:
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		$(CASTKMS_KBUILD_OPTIONS) \
		CONFIG_DRM_CASTKMS_KUNIT_TEST=m modules

build-matrix:
	$(MAKE) module-clean
	$(MAKE) all CONFIG_SND=n
	test ! -e src/castkms_audio.o
	case "$$(modinfo -F depends ./castkms.ko)" in *snd*) false;; esac
	case "$$(modinfo -F softdep ./castkms.ko)" in *snd*) false;; esac
	$(MAKE) module-clean
	$(MAKE) all CASTKMS_BUILD_AUDIO=n
	test ! -e src/castkms_audio.o
	case "$$(modinfo -F depends ./castkms.ko)" in *snd*) false;; esac
	case "$$(modinfo -F softdep ./castkms.ko)" in *snd*) false;; esac
	$(MAKE) module-clean
	$(MAKE) all CASTKMS_BUILD_AUDIO=y
	test -e src/castkms_audio.o

check: check-architecture check-ioctls check-shell
	$(MAKE) -C tools check

check-architecture:
	./scripts/check-architecture.sh

check-ioctls:
	./scripts/check-private-ioctls.sh

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
