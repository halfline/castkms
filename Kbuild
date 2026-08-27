# SPDX-License-Identifier: GPL-2.0-only

obj-m += castkms.o
obj-$(CONFIG_DRM_CASTKMS_KUNIT_TEST) += src/tests/

CASTKMS_BUILD_AUDIO ?= y

ifeq ($(filter y n,$(CASTKMS_BUILD_AUDIO)),)
$(error CASTKMS_BUILD_AUDIO must be y or n)
endif

castkms-y := \
	src/castkms_drv.o \
	src/castkms_file.o \
	src/castkms_framebuffer.o \
	src/castkms_capture_authority.o \
	src/castkms_capture_owner.o \
	src/castkms_grant.o \
	src/castkms_grant_file.o \
	src/castkms_capture_uapi.o \
	src/castkms_capture.o \
	src/castkms_capture_buffer.o \
	src/castkms_capture_cursor.o \
	src/castkms_capture_job.o \
	src/castkms_plane.o \
	src/castkms_topology.o \
	src/castkms_output_buffer.o \
	src/castkms_formats.o \
	src/castkms_crtc.o \
	src/castkms_frame_dispatch.o \
	src/castkms_composer.o \
	src/castkms_writeback.o \
	src/castkms_connector.o \
	src/castkms_connector_uapi.o \
	src/castkms_config.o \
	src/castkms_configfs.o \
	src/castkms_colorop.o \
	src/castkms_luts.o \
	src/castkms_snapshot.o

ifeq ($(CASTKMS_BUILD_AUDIO),y)
ifneq ($(filter y m,$(CONFIG_SND)),)
castkms-y += src/castkms_audio.o
ccflags-y += -DCASTKMS_HAVE_AUDIO=1
endif
endif

ccflags-y += -I$(src)/src -I$(src)/include/uapi
