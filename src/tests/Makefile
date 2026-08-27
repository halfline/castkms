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
