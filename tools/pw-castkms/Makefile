# SPDX-License-Identifier: GPL-2.0-only

CC ?= cc
CFLAGS ?= -O2
WARN_CFLAGS := -std=gnu11 -Wall -Wextra -Werror
CPPFLAGS += -I../../include/uapi

PW_CFLAGS := $(shell pkg-config --cflags libpipewire-0.3)
PW_LIBS := $(shell pkg-config --libs libpipewire-0.3)
DRM_CFLAGS := $(shell pkg-config --cflags libdrm)
DRM_LIBS := $(shell pkg-config --libs libdrm)

.PHONY: all clean

all: pw-castkms

pw-castkms: pw-castkms.c ../../include/uapi/drm/castkms_drm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) $(PW_CFLAGS) $(DRM_CFLAGS) \
		-o $@ $< $(PW_LIBS) $(DRM_LIBS)

clean:
	$(RM) pw-castkms
