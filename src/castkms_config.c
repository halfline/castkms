// SPDX-License-Identifier: GPL-2.0+

#include <linux/slab.h>
#include <linux/printk.h>

#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <kunit/visibility.h>

#include "castkms_config.h"

struct castkms_config *castkms_config_create(const char *dev_name)
{
	struct castkms_config *config;

	config = kzalloc_obj(*config);
	if (!config)
		return ERR_PTR(-ENOMEM);

	config->dev_name = kstrdup_const(dev_name, GFP_KERNEL);
	if (!config->dev_name) {
		kfree(config);
		return ERR_PTR(-ENOMEM);
	}

	INIT_LIST_HEAD(&config->planes);
	INIT_LIST_HEAD(&config->crtcs);
	INIT_LIST_HEAD(&config->encoders);
	INIT_LIST_HEAD(&config->connectors);

	return config;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_create);

struct castkms_config *castkms_config_default_create_outputs(bool enable_cursor,
					       bool enable_writeback,
					       bool enable_overlay,
					       bool enable_plane_pipeline,
					       unsigned int num_outputs)
{
	struct castkms_config *config;
	struct castkms_config_plane *plane_cfg;
	struct castkms_config_crtc *crtc_cfg;
	struct castkms_config_encoder *encoder_cfg;
	struct castkms_config_connector *connector_cfg;
	unsigned int output;
	int n, ret;

	if (!num_outputs || num_outputs > CASTKMS_MAX_OUTPUT_OBJECTS)
		return ERR_PTR(-EINVAL);

	config = castkms_config_create(DEFAULT_DEVICE_NAME);
	if (IS_ERR(config))
		return config;

	for (output = 0; output < num_outputs; output++) {
		plane_cfg = castkms_config_create_plane(config);
		if (IS_ERR(plane_cfg)) {
			ret = PTR_ERR(plane_cfg);
			goto err_alloc;
		}
		castkms_config_plane_set_type(plane_cfg, DRM_PLANE_TYPE_PRIMARY);

		crtc_cfg = castkms_config_create_crtc(config);
		if (IS_ERR(crtc_cfg)) {
			ret = PTR_ERR(crtc_cfg);
			goto err_alloc;
		}
		castkms_config_crtc_set_writeback(crtc_cfg, enable_writeback);

		ret = castkms_config_plane_attach_crtc(plane_cfg, crtc_cfg);
		if (ret)
			goto err_alloc;
		castkms_config_plane_set_default_pipeline(plane_cfg,
							  enable_plane_pipeline);

		if (enable_cursor) {
			plane_cfg = castkms_config_create_plane(config);
			if (IS_ERR(plane_cfg)) {
				ret = PTR_ERR(plane_cfg);
				goto err_alloc;
			}

			castkms_config_plane_set_type(plane_cfg,
						      DRM_PLANE_TYPE_CURSOR);
			castkms_config_plane_set_default_pipeline(plane_cfg,
								  enable_plane_pipeline);

			ret = castkms_config_plane_attach_crtc(plane_cfg,
							       crtc_cfg);
			if (ret)
				goto err_alloc;
		}

		encoder_cfg = castkms_config_create_encoder(config);
		if (IS_ERR(encoder_cfg)) {
			ret = PTR_ERR(encoder_cfg);
			goto err_alloc;
		}

		ret = castkms_config_encoder_attach_crtc(encoder_cfg, crtc_cfg);
		if (ret)
			goto err_alloc;

		connector_cfg = castkms_config_create_connector(config);
		if (IS_ERR(connector_cfg)) {
			ret = PTR_ERR(connector_cfg);
			goto err_alloc;
		}

		ret = castkms_config_connector_attach_encoder(connector_cfg,
							      encoder_cfg);
		if (ret)
			goto err_alloc;
	}

	if (enable_overlay) {
		for (n = 0; n < NUM_OVERLAY_PLANES; n++) {
			plane_cfg = castkms_config_create_plane(config);
			if (IS_ERR(plane_cfg)) {
				ret = PTR_ERR(plane_cfg);
				goto err_alloc;
			}

			castkms_config_plane_set_type(plane_cfg,
						   DRM_PLANE_TYPE_OVERLAY);
			castkms_config_plane_set_default_pipeline(plane_cfg,
								  enable_plane_pipeline);

			castkms_config_for_each_crtc(config, crtc_cfg) {
				ret = castkms_config_plane_attach_crtc(plane_cfg,
								       crtc_cfg);
				if (ret)
					goto err_alloc;
			}
		}
	}

	return config;

err_alloc:
	castkms_config_destroy(config);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_default_create_outputs);

struct castkms_config *castkms_config_default_create(bool enable_cursor,
					       bool enable_writeback,
					       bool enable_overlay,
					       bool enable_plane_pipeline)
{
	return castkms_config_default_create_outputs(enable_cursor,
						     enable_writeback,
						     enable_overlay,
						     enable_plane_pipeline, 1);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_default_create);

void castkms_config_destroy(struct castkms_config *config)
{
	struct castkms_config_plane *plane_cfg, *plane_tmp;
	struct castkms_config_crtc *crtc_cfg, *crtc_tmp;
	struct castkms_config_encoder *encoder_cfg, *encoder_tmp;
	struct castkms_config_connector *connector_cfg, *connector_tmp;

	list_for_each_entry_safe(plane_cfg, plane_tmp, &config->planes, link)
		castkms_config_destroy_plane(plane_cfg);

	list_for_each_entry_safe(crtc_cfg, crtc_tmp, &config->crtcs, link)
		castkms_config_destroy_crtc(crtc_cfg);

	list_for_each_entry_safe(encoder_cfg, encoder_tmp, &config->encoders, link)
		castkms_config_destroy_encoder(encoder_cfg);

	list_for_each_entry_safe(connector_cfg, connector_tmp, &config->connectors, link)
		castkms_config_destroy_connector(connector_cfg);

	kfree_const(config->dev_name);
	kfree(config);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_destroy);

void castkms_config_clear_runtime_objects(struct castkms_config *config)
{
	struct castkms_config_plane *plane_cfg;
	struct castkms_config_crtc *crtc_cfg;
	struct castkms_config_encoder *encoder_cfg;
	struct castkms_config_connector *connector_cfg;

	castkms_config_for_each_plane(config, plane_cfg)
		plane_cfg->plane = NULL;

	castkms_config_for_each_crtc(config, crtc_cfg)
		crtc_cfg->crtc = NULL;

	castkms_config_for_each_encoder(config, encoder_cfg)
		encoder_cfg->encoder = NULL;

	castkms_config_for_each_connector(config, connector_cfg)
		connector_cfg->connector = NULL;
}

static bool valid_plane_number(const struct castkms_config *config)
{
	size_t n_planes;

	n_planes = list_count_nodes((struct list_head *)&config->planes);
	if (!n_planes || n_planes > CASTKMS_MAX_OUTPUT_OBJECTS) {
		pr_info("castkms %s: The number of planes must be between 1 and %u\n",
			config->dev_name, CASTKMS_MAX_OUTPUT_OBJECTS);
		return false;
	}

	return true;
}

static bool valid_planes_for_crtc(const struct castkms_config *config,
				  struct castkms_config_crtc *crtc_cfg)
{
	struct castkms_config_plane *plane_cfg;
	bool has_primary_plane = false;
	bool has_cursor_plane = false;

	castkms_config_for_each_plane(config, plane_cfg) {
		struct castkms_config_crtc *possible_crtc;
		unsigned long idx = 0;
		enum drm_plane_type type;

		type = castkms_config_plane_get_type(plane_cfg);

		castkms_config_plane_for_each_possible_crtc(plane_cfg, idx, possible_crtc) {
			if (possible_crtc != crtc_cfg)
				continue;

			if (type == DRM_PLANE_TYPE_PRIMARY) {
				if (has_primary_plane) {
					pr_info("castkms %s: Multiple primary planes\n",
						config->dev_name);
					return false;
				}

				has_primary_plane = true;
			} else if (type == DRM_PLANE_TYPE_CURSOR) {
				if (has_cursor_plane) {
					pr_info("castkms %s: Multiple cursor planes\n",
						config->dev_name);
					return false;
				}

				has_cursor_plane = true;
			}
		}
	}

	if (!has_primary_plane) {
		pr_info("castkms %s: Primary plane not found\n",
			config->dev_name);
		return false;
	}

	return true;
}

static bool valid_plane_possible_crtcs(const struct castkms_config *config)
{
	struct castkms_config_plane *plane_cfg;

	castkms_config_for_each_plane(config, plane_cfg) {
		if (xa_empty(&plane_cfg->possible_crtcs)) {
			pr_info("castkms %s: All planes must have at least one possible CRTC\n",
				config->dev_name);
			return false;
		}
	}

	return true;
}

static bool valid_crtc_number(const struct castkms_config *config)
{
	size_t n_crtcs;

	n_crtcs = list_count_nodes((struct list_head *)&config->crtcs);
	if (!n_crtcs || n_crtcs > CASTKMS_MAX_OUTPUT_OBJECTS) {
		pr_info("castkms %s: The number of CRTCs must be between 1 and %u\n",
			config->dev_name, CASTKMS_MAX_OUTPUT_OBJECTS);
		return false;
	}

	return true;
}

static bool valid_encoder_number(const struct castkms_config *config)
{
	size_t n_encoders;

	n_encoders = list_count_nodes((struct list_head *)&config->encoders);
	if (!n_encoders || n_encoders > CASTKMS_MAX_OUTPUT_OBJECTS) {
		pr_info("castkms %s: The number of encoders must be between 1 and %u\n",
			config->dev_name, CASTKMS_MAX_OUTPUT_OBJECTS);
		return false;
	}

	return true;
}

static bool valid_encoder_possible_crtcs(const struct castkms_config *config)
{
	struct castkms_config_crtc *crtc_cfg;
	struct castkms_config_encoder *encoder_cfg;

	castkms_config_for_each_encoder(config, encoder_cfg) {
		if (xa_empty(&encoder_cfg->possible_crtcs)) {
			pr_info("castkms %s: All encoders must have at least one possible CRTC\n",
				config->dev_name);
			return false;
		}
	}

	castkms_config_for_each_crtc(config, crtc_cfg) {
		bool crtc_has_encoder = false;

		castkms_config_for_each_encoder(config, encoder_cfg) {
			struct castkms_config_crtc *possible_crtc;
			unsigned long idx = 0;

			castkms_config_encoder_for_each_possible_crtc(encoder_cfg,
								   idx, possible_crtc) {
				if (possible_crtc == crtc_cfg)
					crtc_has_encoder = true;
			}
		}

		if (!crtc_has_encoder) {
			pr_info("castkms %s: All CRTCs must have at least one possible encoder\n",
				config->dev_name);
			return false;
		}
	}

	return true;
}

static bool valid_connector_number(const struct castkms_config *config)
{
	size_t n_connectors;

	n_connectors = list_count_nodes((struct list_head *)&config->connectors);
	if (!n_connectors || n_connectors > CASTKMS_MAX_OUTPUT_OBJECTS) {
		pr_info("castkms %s: The number of connectors must be between 1 and %u\n",
			config->dev_name, CASTKMS_MAX_OUTPUT_OBJECTS);
		return false;
	}

	return true;
}

static bool valid_connector_possible_encoders(const struct castkms_config *config)
{
	struct castkms_config_connector *connector_cfg;

	castkms_config_for_each_connector(config, connector_cfg) {
		if (xa_empty(&connector_cfg->possible_encoders)) {
			pr_info("castkms %s: All connectors must have at least one possible encoder\n",
				config->dev_name);
			return false;
		}
	}

	return true;
}

bool castkms_config_is_valid(const struct castkms_config *config)
{
	struct castkms_config_crtc *crtc_cfg;

	if (!valid_plane_number(config))
		return false;

	if (!valid_crtc_number(config))
		return false;

	if (!valid_encoder_number(config))
		return false;

	if (!valid_connector_number(config))
		return false;

	if (!valid_plane_possible_crtcs(config))
		return false;

	castkms_config_for_each_crtc(config, crtc_cfg) {
		if (!valid_planes_for_crtc(config, crtc_cfg))
			return false;
	}

	if (!valid_encoder_possible_crtcs(config))
		return false;

	if (!valid_connector_possible_encoders(config))
		return false;

	return true;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_is_valid);

static int castkms_config_show(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct drm_device *dev = entry->dev;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_config *config;
	const char *dev_name;
	struct castkms_config_plane *plane_cfg;
	struct castkms_config_crtc *crtc_cfg;
	struct castkms_config_encoder *encoder_cfg;
	struct castkms_config_connector *connector_cfg;
	int idx;

	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;

	config = castkmsdev->config;
	if (!config) {
		drm_dev_exit(idx);
		return -ENODEV;
	}

	dev_name = castkms_config_get_device_name(config);
	seq_printf(m, "dev_name=%s\n", dev_name);

	castkms_config_for_each_plane(config, plane_cfg) {
		seq_puts(m, "plane:\n");
		seq_printf(m, "\ttype=%d\n",
			   castkms_config_plane_get_type(plane_cfg));
	}

	castkms_config_for_each_crtc(config, crtc_cfg) {
		seq_puts(m, "crtc:\n");
		seq_printf(m, "\twriteback=%d\n",
			   castkms_config_crtc_get_writeback(crtc_cfg));
	}

	castkms_config_for_each_encoder(config, encoder_cfg)
		seq_puts(m, "encoder\n");

	castkms_config_for_each_connector(config, connector_cfg) {
		seq_puts(m, "connector:\n");
		seq_printf(m, "\tstatus=%d\n",
			   castkms_config_connector_get_status(connector_cfg));
	}

	drm_dev_exit(idx);

	return 0;
}

static const struct drm_debugfs_info castkms_config_debugfs_list[] = {
	{
		.name = "castkms_config",
		.show = castkms_config_show,
	},
};

void castkms_config_register_debugfs(struct castkms_device *castkms_device)
{
	drm_debugfs_add_files(&castkms_device->drm, castkms_config_debugfs_list,
			      ARRAY_SIZE(castkms_config_debugfs_list));
}

struct castkms_config_plane *castkms_config_create_plane(struct castkms_config *config)
{
	struct castkms_config_plane *plane_cfg;

	plane_cfg = kzalloc_obj(*plane_cfg);
	if (!plane_cfg)
		return ERR_PTR(-ENOMEM);

	plane_cfg->config = config;
	plane_cfg->default_pipeline = false;
	castkms_config_plane_set_type(plane_cfg, DRM_PLANE_TYPE_OVERLAY);
	xa_init_flags(&plane_cfg->possible_crtcs, XA_FLAGS_ALLOC);

	list_add_tail(&plane_cfg->link, &config->planes);

	return plane_cfg;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_create_plane);

void castkms_config_destroy_plane(struct castkms_config_plane *plane_cfg)
{
	xa_destroy(&plane_cfg->possible_crtcs);
	list_del(&plane_cfg->link);
	kfree(plane_cfg);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_destroy_plane);

int __must_check castkms_config_plane_attach_crtc(struct castkms_config_plane *plane_cfg,
					       struct castkms_config_crtc *crtc_cfg)
{
	struct castkms_config_crtc *possible_crtc;
	unsigned long idx = 0;
	u32 crtc_idx = 0;

	if (plane_cfg->config != crtc_cfg->config)
		return -EINVAL;

	castkms_config_plane_for_each_possible_crtc(plane_cfg, idx, possible_crtc) {
		if (possible_crtc == crtc_cfg)
			return -EEXIST;
	}

	return xa_alloc(&plane_cfg->possible_crtcs, &crtc_idx, crtc_cfg,
			xa_limit_32b, GFP_KERNEL);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_plane_attach_crtc);

void castkms_config_plane_detach_crtc(struct castkms_config_plane *plane_cfg,
				   struct castkms_config_crtc *crtc_cfg)
{
	struct castkms_config_crtc *possible_crtc;
	unsigned long idx = 0;

	castkms_config_plane_for_each_possible_crtc(plane_cfg, idx, possible_crtc) {
		if (possible_crtc == crtc_cfg)
			xa_erase(&plane_cfg->possible_crtcs, idx);
	}
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_plane_detach_crtc);

struct castkms_config_crtc *castkms_config_create_crtc(struct castkms_config *config)
{
	struct castkms_config_crtc *crtc_cfg;

	crtc_cfg = kzalloc_obj(*crtc_cfg);
	if (!crtc_cfg)
		return ERR_PTR(-ENOMEM);

	crtc_cfg->config = config;
	castkms_config_crtc_set_writeback(crtc_cfg, false);

	list_add_tail(&crtc_cfg->link, &config->crtcs);

	return crtc_cfg;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_create_crtc);

void castkms_config_destroy_crtc(struct castkms_config_crtc *crtc_cfg)
{
	struct castkms_config *config = crtc_cfg->config;
	struct castkms_config_plane *plane_cfg;
	struct castkms_config_encoder *encoder_cfg;

	castkms_config_for_each_plane(config, plane_cfg)
		castkms_config_plane_detach_crtc(plane_cfg, crtc_cfg);

	castkms_config_for_each_encoder(config, encoder_cfg)
		castkms_config_encoder_detach_crtc(encoder_cfg, crtc_cfg);

	list_del(&crtc_cfg->link);
	kfree(crtc_cfg);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_destroy_crtc);

/**
 * castkms_crtc_get_plane() - Return the first attached plane to a CRTC with
 * the specific type
 * @crtc_cfg: Only find planes attached to this CRTC
 * @type: Plane type to search
 *
 * Returns:
 * The first plane found attached to @crtc_cfg with the type @type.
 */
static struct castkms_config_plane *
castkms_crtc_get_plane(struct castkms_config_crtc *crtc_cfg,
		       enum drm_plane_type type)
{
	struct castkms_config *config = crtc_cfg->config;
	struct castkms_config_plane *plane_cfg;
	struct castkms_config_crtc *possible_crtc;
	enum drm_plane_type current_type;
	unsigned long idx = 0;

	castkms_config_for_each_plane(config, plane_cfg) {
		current_type = castkms_config_plane_get_type(plane_cfg);

		castkms_config_plane_for_each_possible_crtc(plane_cfg, idx, possible_crtc) {
			if (possible_crtc == crtc_cfg && current_type == type)
				return plane_cfg;
		}
	}

	return NULL;
}

struct castkms_config_plane *
castkms_config_crtc_primary_plane(struct castkms_config_crtc *crtc_cfg)
{
	return castkms_crtc_get_plane(crtc_cfg, DRM_PLANE_TYPE_PRIMARY);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_crtc_primary_plane);

struct castkms_config_plane *
castkms_config_crtc_cursor_plane(struct castkms_config_crtc *crtc_cfg)
{
	return castkms_crtc_get_plane(crtc_cfg, DRM_PLANE_TYPE_CURSOR);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_crtc_cursor_plane);

struct castkms_config_encoder *castkms_config_create_encoder(struct castkms_config *config)
{
	struct castkms_config_encoder *encoder_cfg;

	encoder_cfg = kzalloc_obj(*encoder_cfg);
	if (!encoder_cfg)
		return ERR_PTR(-ENOMEM);

	encoder_cfg->config = config;
	xa_init_flags(&encoder_cfg->possible_crtcs, XA_FLAGS_ALLOC);

	list_add_tail(&encoder_cfg->link, &config->encoders);

	return encoder_cfg;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_create_encoder);

void castkms_config_destroy_encoder(struct castkms_config_encoder *encoder_cfg)
{
	struct castkms_config *config = encoder_cfg->config;
	struct castkms_config_connector *connector_cfg;

	castkms_config_for_each_connector(config, connector_cfg)
		castkms_config_connector_detach_encoder(connector_cfg, encoder_cfg);

	xa_destroy(&encoder_cfg->possible_crtcs);
	list_del(&encoder_cfg->link);
	kfree(encoder_cfg);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_destroy_encoder);

int __must_check castkms_config_encoder_attach_crtc(struct castkms_config_encoder *encoder_cfg,
						 struct castkms_config_crtc *crtc_cfg)
{
	struct castkms_config_crtc *possible_crtc;
	unsigned long idx = 0;
	u32 crtc_idx = 0;

	if (encoder_cfg->config != crtc_cfg->config)
		return -EINVAL;

	castkms_config_encoder_for_each_possible_crtc(encoder_cfg, idx, possible_crtc) {
		if (possible_crtc == crtc_cfg)
			return -EEXIST;
	}

	return xa_alloc(&encoder_cfg->possible_crtcs, &crtc_idx, crtc_cfg,
			xa_limit_32b, GFP_KERNEL);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_encoder_attach_crtc);

void castkms_config_encoder_detach_crtc(struct castkms_config_encoder *encoder_cfg,
				     struct castkms_config_crtc *crtc_cfg)
{
	struct castkms_config_crtc *possible_crtc;
	unsigned long idx = 0;

	castkms_config_encoder_for_each_possible_crtc(encoder_cfg, idx, possible_crtc) {
		if (possible_crtc == crtc_cfg)
			xa_erase(&encoder_cfg->possible_crtcs, idx);
	}
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_encoder_detach_crtc);

struct castkms_config_connector *castkms_config_create_connector(struct castkms_config *config)
{
	struct castkms_config_connector *connector_cfg;

	connector_cfg = kzalloc_obj(*connector_cfg);
	if (!connector_cfg)
		return ERR_PTR(-ENOMEM);

	connector_cfg->config = config;
	connector_cfg->status = connector_status_disconnected;
	xa_init_flags(&connector_cfg->possible_encoders, XA_FLAGS_ALLOC);

	list_add_tail(&connector_cfg->link, &config->connectors);

	return connector_cfg;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_create_connector);

void castkms_config_destroy_connector(struct castkms_config_connector *connector_cfg)
{
	xa_destroy(&connector_cfg->possible_encoders);
	list_del(&connector_cfg->link);
	kfree(connector_cfg);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_destroy_connector);

int __must_check castkms_config_connector_attach_encoder(struct castkms_config_connector *connector_cfg,
						      struct castkms_config_encoder *encoder_cfg)
{
	struct castkms_config_encoder *possible_encoder;
	unsigned long idx = 0;
	u32 encoder_idx = 0;

	if (connector_cfg->config != encoder_cfg->config)
		return -EINVAL;

	castkms_config_connector_for_each_possible_encoder(connector_cfg, idx,
							possible_encoder) {
		if (possible_encoder == encoder_cfg)
			return -EEXIST;
	}

	return xa_alloc(&connector_cfg->possible_encoders, &encoder_idx,
			encoder_cfg, xa_limit_32b, GFP_KERNEL);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_connector_attach_encoder);

void castkms_config_connector_detach_encoder(struct castkms_config_connector *connector_cfg,
					  struct castkms_config_encoder *encoder_cfg)
{
	struct castkms_config_encoder *possible_encoder;
	unsigned long idx = 0;

	castkms_config_connector_for_each_possible_encoder(connector_cfg, idx,
							possible_encoder) {
		if (possible_encoder == encoder_cfg)
			xa_erase(&connector_cfg->possible_encoders, idx);
	}
}
EXPORT_SYMBOL_IF_KUNIT(castkms_config_connector_detach_encoder);
