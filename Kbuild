# SPDX-License-Identifier: GPL-2.0-only

obj-m += castkms.o
obj-$(CONFIG_DRM_CASTKMS_KUNIT_TEST) += src/tests/

castkms-y := \
	src/castkms_drv.o \
	src/castkms_capture.o \
	src/castkms_plane.o \
	src/castkms_output.o \
	src/castkms_output_buffer.o \
	src/castkms_formats.o \
	src/castkms_crtc.o \
	src/castkms_composer.o \
	src/castkms_writeback.o \
	src/castkms_connector.o \
	src/castkms_config.o \
	src/castkms_configfs.o \
	src/castkms_colorop.o \
	src/castkms_luts.o

ccflags-y += -I$(src)/src -I$(src)/include/uapi
