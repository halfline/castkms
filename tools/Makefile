# SPDX-License-Identifier: GPL-2.0-only

CC ?= cc
CFLAGS ?= -O2
WARN_CFLAGS := -std=gnu11 -Wall -Wextra -Werror
CPPFLAGS += -I../include/uapi

.PHONY: all clean

all: castkms-capture-test castkms-edid-test
	$(MAKE) -C pw-castkms

castkms-capture-test: castkms-capture-test.c \
		virtualscreen-edid.h \
		../include/uapi/drm/castkms_drm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) -o $@ $<

castkms-edid-test: castkms-edid-test.c virtualscreen-edid.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) -o $@ $<

check: castkms-edid-test
	./castkms-edid-test

clean:
	$(RM) castkms-capture-test castkms-edid-test
	$(MAKE) -C pw-castkms clean
