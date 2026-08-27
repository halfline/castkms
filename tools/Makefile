# SPDX-License-Identifier: GPL-2.0-only

CC ?= cc
CFLAGS ?= -O2
WARN_CFLAGS := -std=gnu11 -Wall -Wextra -Werror
CPPFLAGS += -I../include/uapi

.PHONY: all check clean

all: castkms-grant-test

castkms-grant-test: castkms-grant-test.c \
		castkms-test-drm.c castkms-test-drm.h \
		../include/uapi/drm/castkms_drm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) -o $@ \
		castkms-grant-test.c castkms-test-drm.c

check: all
	./castkms-grant-test -h >/dev/null 2>&1

clean:
	$(RM) castkms-grant-test
