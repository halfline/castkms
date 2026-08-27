# SPDX-License-Identifier: GPL-2.0-only

ccflags-y += -I$(src)/../../include/uapi

castkms-kunit-tests-y := \
	castkms_core_client_test.o \
	castkms_snapshot_test.o \
	castkms_config_test.o \
	castkms_format_test.o \
	castkms_color_test.o \
	castkms_capture_buffer_test.o \
	castkms_frame_dispatch_test.o \
	castkms_grant_test.o

obj-$(CONFIG_DRM_CASTKMS_KUNIT_TEST) += castkms-kunit-tests.o

ifeq ($(CASTKMS_BUILD_CEC),y)
ifneq ($(wildcard $(srctree)/include/drm/display/drm_hdmi_cec_helper.h),)
ifneq ($(filter y m,$(CONFIG_DRM_DISPLAY_HDMI_CEC_HELPER)),)
castkms-kunit-tests-y += castkms_cec_test.o
ccflags-y += -DCASTKMS_HAVE_CEC=1
endif
endif
endif
