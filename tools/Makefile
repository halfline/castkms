# SPDX-License-Identifier: GPL-2.0-only

CC ?= cc
CFLAGS ?= -O2
WARN_CFLAGS := -std=gnu11 -Wall -Wextra -Werror
CPPFLAGS += -I../include/uapi
ALSA_CFLAGS := $(shell pkg-config --cflags alsa 2>/dev/null)
ALSA_LIBS := $(shell pkg-config --libs alsa 2>/dev/null)

.PHONY: all check clean

all: castkms-capture-test castkms-grant-test castkms-attach \
	castkms-edid-test
	$(MAKE) -C pw-castkms
ifneq ($(ALSA_LIBS),)
	$(MAKE) castkms-audio-test
endif

castkms-capture-test: castkms-capture-test.c \
		castkms-test-drm.c castkms-test-drm.h \
		virtualscreen-edid.h \
		../include/uapi/drm/castkms_drm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) -o $@ \
		castkms-capture-test.c castkms-test-drm.c

castkms-grant-test: castkms-grant-test.c \
		castkms-test-drm.c castkms-test-drm.h \
		../include/uapi/drm/castkms_drm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) -o $@ \
		castkms-grant-test.c castkms-test-drm.c

castkms-attach: castkms-attach.c \
		castkms-test-drm.c castkms-test-drm.h \
		virtualscreen-edid.h \
		../include/uapi/drm/castkms_drm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) -o $@ \
		castkms-attach.c castkms-test-drm.c

castkms-edid-test: castkms-edid-test.c virtualscreen-edid.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) -o $@ $<

check: all
	./castkms-capture-test -h >/dev/null 2>&1
	./castkms-grant-test -h >/dev/null 2>&1
	./castkms-attach -h >/dev/null 2>&1
	./castkms-edid-test
ifneq ($(ALSA_LIBS),)
	./castkms-audio-test -h >/dev/null 2>&1
endif
	$(MAKE) -C pw-castkms check

castkms-audio-test: castkms-audio-test.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) $(ALSA_CFLAGS) \
		-o $@ $< $(ALSA_LIBS) -lm

clean:
	$(RM) castkms-capture-test castkms-grant-test castkms-attach \
		castkms-edid-test castkms-audio-test
	$(MAKE) -C pw-castkms clean
