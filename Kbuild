# SPDX-License-Identifier: GPL-2.0-only

obj-m += castkms.o
obj-$(CONFIG_DRM_CASTKMS_KUNIT_TEST) += src/tests/

CASTKMS_BUILD_AUDIO ?= y
CASTKMS_BUILD_CEC ?= y

ifeq ($(filter y n,$(CASTKMS_BUILD_AUDIO)),)
$(error CASTKMS_BUILD_AUDIO must be y or n)
endif

ifeq ($(filter y n,$(CASTKMS_BUILD_CEC)),)
$(error CASTKMS_BUILD_CEC must be y or n)
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
ifeq ($(CASTKMS_BUILD_CEC),y)
ifneq ($(wildcard $(srctree)/include/drm/display/drm_hdmi_cec_helper.h),)
ifneq ($(filter y m,$(CONFIG_DRM_DISPLAY_HDMI_CEC_HELPER)),)
castkms-y += src/castkms_cec_core.o src/castkms_cec_uapi.o
ccflags-y += -DCASTKMS_HAVE_CEC=1
endif
endif
endif

ccflags-y += -I$(src)/src -I$(src)/include/uapi
