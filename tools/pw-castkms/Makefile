# SPDX-License-Identifier: GPL-2.0-only

CC ?= cc
CFLAGS ?= -O2
WARN_CFLAGS := -std=gnu11 -Wall -Wextra -Werror
CPPFLAGS += -I../../include/uapi

PW_CFLAGS := $(shell pkg-config --cflags libpipewire-0.3)
PW_LIBS := $(shell pkg-config --libs libpipewire-0.3)
DRM_CFLAGS := $(shell pkg-config --cflags libdrm)
DRM_LIBS := $(shell pkg-config --libs libdrm)

HAS_GTK4 := $(shell pkg-config --exists gtk4 2>/dev/null && echo 1)
ifeq ($(HAS_GTK4),1)
GTK4_CFLAGS := $(shell pkg-config --cflags gtk4)
GTK4_LIBS := $(shell pkg-config --libs gtk4)
VIEW_TARGET := pw-castkms-view
endif

.PHONY: all check clean

all: pw-castkms pw-castkms-test $(VIEW_TARGET)

PW_CASTKMS_SOURCES := pw-castkms.c castkms.c castkms-buffer.c pipewire.c
PW_CASTKMS_HEADERS := pw-castkms.h ../virtualscreen-edid.h \
		../../include/uapi/drm/castkms_drm.h

pw-castkms: $(PW_CASTKMS_SOURCES) $(PW_CASTKMS_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) $(PW_CFLAGS) $(DRM_CFLAGS) \
		-o $@ $(PW_CASTKMS_SOURCES) $(PW_LIBS) $(DRM_LIBS)

pw-castkms-test: pw-castkms-test.c
	$(CC) $(CFLAGS) $(WARN_CFLAGS) $(PW_CFLAGS) $(DRM_CFLAGS) \
		-o $@ $< $(PW_LIBS)

pw-castkms-view: pw-castkms-view.c
	$(CC) $(CFLAGS) $(WARN_CFLAGS) $(PW_CFLAGS) $(DRM_CFLAGS) \
		$(GTK4_CFLAGS) -o $@ $< $(PW_LIBS) $(GTK4_LIBS)

check: all
	./pw-castkms -h >/dev/null 2>&1
	./pw-castkms-test -h >/dev/null 2>&1
ifeq ($(HAS_GTK4),1)
	./pw-castkms-view -h >/dev/null 2>&1
endif

clean:
	$(RM) pw-castkms pw-castkms-test pw-castkms-view
