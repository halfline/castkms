# SPDX-License-Identifier: GPL-2.0-only

CC ?= cc
CFLAGS ?= -O2
WARN_CFLAGS := -std=gnu11 -Wall -Wextra -Werror
CPPFLAGS += -I../include/uapi

.PHONY: all clean

ALSA_CFLAGS := $(shell pkg-config --cflags alsa 2>/dev/null)
ALSA_LIBS := $(shell pkg-config --libs alsa 2>/dev/null)

all: castkms-capture-test castkms-edid-test castkms-cec-test
	$(MAKE) -C pw-castkms
ifneq ($(ALSA_LIBS),)
	$(MAKE) castkms-audio-test
endif

castkms-capture-test: castkms-capture-test.c \
		virtualscreen-edid.h \
		../include/uapi/drm/castkms_drm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) -o $@ $<

castkms-edid-test: castkms-edid-test.c virtualscreen-edid.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) -o $@ $<

castkms-cec-test: castkms-cec-test.c virtualscreen-edid.h \
		../include/uapi/drm/castkms_drm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) -o $@ $<

check: castkms-edid-test
	./castkms-edid-test

castkms-audio-test: castkms-audio-test.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) $(ALSA_CFLAGS) \
		-o $@ $< $(ALSA_LIBS) -lm

clean:
	$(RM) castkms-capture-test castkms-edid-test castkms-cec-test castkms-audio-test
	$(MAKE) -C pw-castkms clean
