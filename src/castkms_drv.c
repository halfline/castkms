// SPDX-License-Identifier: GPL-2.0+

/**
 * DOC: castkms virtual display capture
 *
 * CASTKMS is a software KMS sink for attaching virtual monitors and exporting
 * their presentation as synchronized frame capture. Its primary userspace path
 * is monitor attachment, compositor modesetting, capture, and PipeWire export;
 * VKMS-derived CRC, writeback, and configurable topology remain development and
 * compatibility facilities rather than the product interface.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/device/faux.h>
#include <linux/dma-mapping.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_gem.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_colorop.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_vblank.h>

#include <drm/castkms_drm.h>

#include "castkms_capture.h"
#include "castkms_config.h"
#include "castkms_configfs.h"
#include "castkms_drv.h"

#define DRIVER_NAME	"castkms"
#define DRIVER_DESC	"CASTKMS virtual display capture"
#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

static struct castkms_config *default_config;

static bool enable_cursor = true;
module_param_named(enable_cursor, enable_cursor, bool, 0444);
MODULE_PARM_DESC(enable_cursor, "Enable/Disable cursor support");

static bool enable_writeback = true;
module_param_named(enable_writeback, enable_writeback, bool, 0444);
MODULE_PARM_DESC(enable_writeback, "Enable/Disable writeback connector support");

static bool enable_overlay;
module_param_named(enable_overlay, enable_overlay, bool, 0444);
MODULE_PARM_DESC(enable_overlay, "Enable/Disable overlay support");

static bool enable_plane_pipeline;
module_param_named(enable_plane_pipeline, enable_plane_pipeline, bool, 0444);
MODULE_PARM_DESC(enable_plane_pipeline, "Enable/Disable plane pipeline support");

static bool create_default_dev = true;
module_param_named(create_default_dev, create_default_dev, bool, 0444);
MODULE_PARM_DESC(create_default_dev, "Create or not the default CASTKMS device");

static unsigned int max_outputs = 1;
module_param_named(max_outputs, max_outputs, uint, 0444);
MODULE_PARM_DESC(max_outputs,
		 "Number of display outputs on the default device (1-31)");

DEFINE_DRM_GEM_FOPS(castkms_driver_fops);

static const struct drm_ioctl_desc castkms_ioctls[] = {
	DRM_IOCTL_DEF_DRV(CASTKMS_CAPTURE_QUERY_CAPS,
			  castkms_capture_query_caps_ioctl, 0),
	DRM_IOCTL_DEF_DRV(CASTKMS_CAPTURE_START,
			  castkms_capture_start_ioctl, DRM_ROOT_ONLY),
	DRM_IOCTL_DEF_DRV(CASTKMS_CAPTURE_STOP,
			  castkms_capture_stop_ioctl, DRM_ROOT_ONLY),
	DRM_IOCTL_DEF_DRV(CASTKMS_CAPTURE_REGISTER_BUFFER,
			  castkms_capture_register_buffer_ioctl, DRM_ROOT_ONLY),
	DRM_IOCTL_DEF_DRV(CASTKMS_CAPTURE_UNREGISTER_BUFFER,
			  castkms_capture_unregister_buffer_ioctl, DRM_ROOT_ONLY),
	DRM_IOCTL_DEF_DRV(CASTKMS_CAPTURE_QUEUE_BUFFER,
			  castkms_capture_queue_buffer_ioctl, DRM_ROOT_ONLY),
	DRM_IOCTL_DEF_DRV(CASTKMS_CAPTURE_SET_OUTPUT_EDID,
			  castkms_capture_set_output_edid_ioctl, DRM_ROOT_ONLY),
	DRM_IOCTL_DEF_DRV(CASTKMS_CAPTURE_ATTACH_MONITOR,
			  castkms_capture_attach_monitor_ioctl, DRM_ROOT_ONLY),
	DRM_IOCTL_DEF_DRV(CASTKMS_CAPTURE_DETACH_MONITOR,
			  castkms_capture_detach_monitor_ioctl, DRM_ROOT_ONLY),
	DRM_IOCTL_DEF_DRV(CASTKMS_CAPTURE_READ_CURSOR_BITMAP,
			  castkms_capture_read_cursor_bitmap_ioctl, DRM_ROOT_ONLY),
};

static void castkms_atomic_commit_tail(struct drm_atomic_state *old_state)
{
	struct drm_device *dev = old_state->dev;
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_crtc_state;
	int i;

	drm_atomic_helper_commit_modeset_disables(dev, old_state);

	drm_atomic_helper_commit_planes(dev, old_state, 0);

	drm_atomic_helper_commit_modeset_enables(dev, old_state);

	drm_atomic_helper_fake_vblank(old_state);

	drm_atomic_helper_commit_hw_done(old_state);

	drm_atomic_helper_wait_for_flip_done(dev, old_state);

	for_each_old_crtc_in_state(old_state, crtc, old_crtc_state, i) {
		struct castkms_crtc_state *castkms_state = to_castkms_crtc_state(old_crtc_state);

		flush_work(&castkms_state->composer_work);
	}

	drm_atomic_helper_cleanup_planes(dev, old_state);
}

static const struct drm_driver castkms_driver = {
	.driver_features	= DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM |
			  DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE |
			  DRIVER_CURSOR_HOTSPOT,
	.ioctls			= castkms_ioctls,
	.num_ioctls		= ARRAY_SIZE(castkms_ioctls),
	.open			= castkms_capture_file_open,
	.postclose		= castkms_capture_file_close,
	.fops			= &castkms_driver_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,

	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.major			= DRIVER_MAJOR,
	.minor			= DRIVER_MINOR,
};

static int castkms_atomic_check(struct drm_device *dev, struct drm_atomic_state *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *new_crtc_state;
	int i;

	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		size_t gamma_lut_length;

		if (!new_crtc_state->gamma_lut || !new_crtc_state->color_mgmt_changed)
			continue;

		gamma_lut_length = new_crtc_state->gamma_lut->length;
		if (!gamma_lut_length ||
		    gamma_lut_length % sizeof(struct drm_color_lut) ||
		    gamma_lut_length / sizeof(struct drm_color_lut) > CASTKMS_LUT_SIZE)
			return -EINVAL;
	}

	return drm_atomic_helper_check(dev, state);
}

static const struct drm_mode_config_funcs castkms_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = castkms_atomic_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs castkms_mode_config_helpers = {
	.atomic_commit_tail = castkms_atomic_commit_tail,
};

static int castkms_modeset_init(struct castkms_device *castkmsdev)
{
	struct drm_device *dev = &castkmsdev->drm;
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	dev->mode_config.funcs = &castkms_mode_funcs;
	dev->mode_config.min_width = XRES_MIN;
	dev->mode_config.min_height = YRES_MIN;
	dev->mode_config.max_width = XRES_MAX;
	dev->mode_config.max_height = YRES_MAX;
	dev->mode_config.normalize_zpos = true;
	dev->mode_config.cursor_width = 512;
	dev->mode_config.cursor_height = 512;
	/*
	 * FIXME: There's a confusion between bpp and depth between this and
	 * fbdev helpers. We have to go with 0, meaning "pick the default",
	 * which is XRGB8888 in all cases.
	 */
	dev->mode_config.preferred_depth = 0;
	dev->mode_config.helper_private = &castkms_mode_config_helpers;

	return castkms_output_init(castkmsdev);
}

int castkms_create(struct castkms_config *config)
{
	int ret;
	struct faux_device *fdev;
	struct castkms_device *castkms_device;
	const char *dev_name;

	if (config->dev)
		return -EBUSY;

	dev_name = castkms_config_get_device_name(config);
	fdev = faux_device_create(dev_name, NULL, NULL);
	if (!fdev)
		return -ENODEV;

	if (!devres_open_group(&fdev->dev, NULL, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto out_unregister;
	}

	castkms_device = devm_drm_dev_alloc(&fdev->dev, &castkms_driver,
					 struct castkms_device, drm);
	if (IS_ERR(castkms_device)) {
		ret = PTR_ERR(castkms_device);
		goto out_devres;
	}
	castkms_device->faux_dev = fdev;
	castkms_device->config = config;
	config->dev = castkms_device;
	mutex_init(&castkms_device->attach_lock);

	ret = dma_coerce_mask_and_coherent(castkms_device->drm.dev,
					   DMA_BIT_MASK(64));

	if (ret) {
		DRM_ERROR("Could not initialize DMA support\n");
		goto out_devres;
	}

	ret = drm_vblank_init(&castkms_device->drm,
			      castkms_config_get_num_crtcs(config));
	if (ret) {
		DRM_ERROR("Failed to vblank\n");
		goto out_devres;
	}

	ret = castkms_modeset_init(castkms_device);
	if (ret)
		goto out_devres;

	castkms_config_register_debugfs(castkms_device);

	ret = drm_dev_register(&castkms_device->drm, 0);
	if (ret)
		goto out_devres;

	drm_client_setup(&castkms_device->drm, NULL);

	return 0;

out_devres:
	castkms_config_clear_runtime_objects(config);
	config->dev = NULL;
	devres_release_group(&fdev->dev, NULL);
out_unregister:
	faux_device_destroy(fdev);
	return ret;
}

static int __init castkms_init(void)
{
	int ret;
	struct castkms_config *config;

	ret = castkms_configfs_register();
	if (ret)
		return ret;

	if (!create_default_dev)
		return 0;

	if (!max_outputs || max_outputs > CASTKMS_MAX_OUTPUT_OBJECTS) {
		DRM_ERROR("max_outputs must be between 1 and %u\n",
			  CASTKMS_MAX_OUTPUT_OBJECTS);
		ret = -EINVAL;
		goto err_configfs;
	}

	config = castkms_config_default_create_outputs(enable_cursor,
						       enable_writeback,
						       enable_overlay,
						       enable_plane_pipeline,
						       max_outputs);
	if (IS_ERR(config)) {
		ret = PTR_ERR(config);
		goto err_configfs;
	}

	ret = castkms_create(config);
	if (ret) {
		castkms_config_destroy(config);
		goto err_configfs;
	}

	default_config = config;

	return 0;

err_configfs:
	castkms_configfs_unregister();
	return ret;
}

void castkms_destroy(struct castkms_config *config)
{
	struct castkms_device *castkms_device;
	struct faux_device *fdev;

	if (!config->dev) {
		DRM_INFO("castkms_device is NULL.\n");
		return;
	}

	castkms_device = config->dev;
	fdev = castkms_device->faux_dev;

	drm_dev_unplug(&castkms_device->drm);
	drm_atomic_helper_shutdown(&castkms_device->drm);
	castkms_config_clear_runtime_objects(config);
	castkms_device->config = NULL;
	config->dev = NULL;
	devres_release_group(&fdev->dev, NULL);
	faux_device_destroy(fdev);
}

static void __exit castkms_exit(void)
{
	castkms_configfs_unregister();

	if (!default_config)
		return;

	castkms_destroy(default_config);
	castkms_config_destroy(default_config);
}

module_init(castkms_init);
module_exit(castkms_exit);

MODULE_AUTHOR("Haneen Mohammed <hamohammed.sa@gmail.com>");
MODULE_AUTHOR("Rodrigo Siqueira <rodrigosiqueiramelo@gmail.com>");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
