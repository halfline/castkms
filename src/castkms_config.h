/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_CONFIG_H_
#define _CASTKMS_CONFIG_H_

#include <linux/list.h>
#include <linux/types.h>
#include <linux/xarray.h>

#include <drm/drm_connector.h>
#include <drm/drm_plane.h>

struct castkms_connector;
struct castkms_device;
struct castkms_output;
struct castkms_plane;
struct drm_encoder;

/**
 * struct castkms_config - General configuration for CASTKMS driver
 *
 * @dev_name: Name of the device
 * @planes: List of planes configured for the device
 * @crtcs: List of CRTCs configured for the device
 * @encoders: List of encoders configured for the device
 * @connectors: List of connectors configured for the device
 * @dev: Used to store the current CASTKMS device. Only set when the device is instantiated.
 */
struct castkms_config {
	const char *dev_name;
	struct list_head planes;
	struct list_head crtcs;
	struct list_head encoders;
	struct list_head connectors;
	struct castkms_device *dev;
};

/**
 * struct castkms_config_plane
 *
 * @link: Link to the others planes in castkms_config
 * @config: The castkms_config this plane belongs to
 * @type: Type of the plane. The creator of configuration needs to ensures that
 *        at least one primary plane is present.
 * @possible_crtcs: Array of CRTCs that can be used with this plane
 * @plane: Internal usage. This pointer should never be considered as valid.
 *         It can be used to store a temporary reference to a CASTKMS plane during
 *         device creation. This pointer is not managed by the configuration and
 *         must be managed by other means.
 */
struct castkms_config_plane {
	struct list_head link;
	struct castkms_config *config;

	enum drm_plane_type type;
	struct xarray possible_crtcs;
	bool default_pipeline;

	/* Internal usage */
	struct castkms_plane *plane;
};

/**
 * struct castkms_config_crtc
 *
 * @link: Link to the others CRTCs in castkms_config
 * @config: The castkms_config this CRTC belongs to
 * @writeback: If true, a writeback buffer can be attached to the CRTC
 * @crtc: Internal usage. This pointer should never be considered as valid.
 *        It can be used to store a temporary reference to a CASTKMS CRTC during
 *        device creation. This pointer is not managed by the configuration and
 *        must be managed by other means.
 */
struct castkms_config_crtc {
	struct list_head link;
	struct castkms_config *config;

	bool writeback;

	/* Internal usage */
	struct castkms_output *crtc;
};

/**
 * struct castkms_config_encoder
 *
 * @link: Link to the others encoders in castkms_config
 * @config: The castkms_config this CRTC belongs to
 * @possible_crtcs: Array of CRTCs that can be used with this encoder
 * @encoder: Internal usage. This pointer should never be considered as valid.
 *           It can be used to store a temporary reference to a CASTKMS encoder
 *           during device creation. This pointer is not managed by the
 *           configuration and must be managed by other means.
 */
struct castkms_config_encoder {
	struct list_head link;
	struct castkms_config *config;

	struct xarray possible_crtcs;

	/* Internal usage */
	struct drm_encoder *encoder;
};

/**
 * struct castkms_config_connector
 *
 * @link: Link to the others connector in castkms_config
 * @config: The castkms_config this connector belongs to
 * @status: Status (connected, disconnected...) of the connector
 * @possible_encoders: Array of encoders that can be used with this connector
 * @connector: Internal usage. This pointer should never be considered as valid.
 *             It can be used to store a temporary reference to a CASTKMS connector
 *             during device creation. This pointer is not managed by the
 *             configuration and must be managed by other means.
 */
struct castkms_config_connector {
	struct list_head link;
	struct castkms_config *config;

	enum drm_connector_status status;
	struct xarray possible_encoders;

	/* Internal usage */
	struct castkms_connector *connector;
};

/**
 * castkms_config_for_each_plane - Iterate over the castkms_config planes
 * @config: &struct castkms_config pointer
 * @plane_cfg: &struct castkms_config_plane pointer used as cursor
 */
#define castkms_config_for_each_plane(config, plane_cfg) \
	list_for_each_entry((plane_cfg), &(config)->planes, link)

/**
 * castkms_config_for_each_crtc - Iterate over the castkms_config CRTCs
 * @config: &struct castkms_config pointer
 * @crtc_cfg: &struct castkms_config_crtc pointer used as cursor
 */
#define castkms_config_for_each_crtc(config, crtc_cfg) \
	list_for_each_entry((crtc_cfg), &(config)->crtcs, link)

/**
 * castkms_config_for_each_encoder - Iterate over the castkms_config encoders
 * @config: &struct castkms_config pointer
 * @encoder_cfg: &struct castkms_config_encoder pointer used as cursor
 */
#define castkms_config_for_each_encoder(config, encoder_cfg) \
	list_for_each_entry((encoder_cfg), &(config)->encoders, link)

/**
 * castkms_config_for_each_connector - Iterate over the castkms_config connectors
 * @config: &struct castkms_config pointer
 * @connector_cfg: &struct castkms_config_connector pointer used as cursor
 */
#define castkms_config_for_each_connector(config, connector_cfg) \
	list_for_each_entry((connector_cfg), &(config)->connectors, link)

/**
 * castkms_config_plane_for_each_possible_crtc - Iterate over the castkms_config_plane
 * possible CRTCs
 * @plane_cfg: &struct castkms_config_plane pointer
 * @idx: Index of the cursor
 * @possible_crtc: &struct castkms_config_crtc pointer used as cursor
 */
#define castkms_config_plane_for_each_possible_crtc(plane_cfg, idx, possible_crtc) \
	xa_for_each(&(plane_cfg)->possible_crtcs, idx, (possible_crtc))

/**
 * castkms_config_encoder_for_each_possible_crtc - Iterate over the
 * castkms_config_encoder possible CRTCs
 * @encoder_cfg: &struct castkms_config_encoder pointer
 * @idx: Index of the cursor
 * @possible_crtc: &struct castkms_config_crtc pointer used as cursor
 */
#define castkms_config_encoder_for_each_possible_crtc(encoder_cfg, idx, possible_crtc) \
	xa_for_each(&(encoder_cfg)->possible_crtcs, idx, (possible_crtc))

/**
 * castkms_config_connector_for_each_possible_encoder - Iterate over the
 * castkms_config_connector possible encoders
 * @connector_cfg: &struct castkms_config_connector pointer
 * @idx: Index of the cursor
 * @possible_encoder: &struct castkms_config_encoder pointer used as cursor
 */
#define castkms_config_connector_for_each_possible_encoder(connector_cfg, idx, possible_encoder) \
	xa_for_each(&(connector_cfg)->possible_encoders, idx, (possible_encoder))

/**
 * castkms_config_create() - Create a new CASTKMS configuration
 * @dev_name: Name of the device
 *
 * Returns:
 * The new castkms_config or an error. Call castkms_config_destroy() to free the
 * returned configuration.
 */
struct castkms_config *castkms_config_create(const char *dev_name);

/**
 * castkms_config_default_create() - Create the configuration for the default device
 * @enable_cursor: Create or not a cursor plane
 * @enable_writeback: Create or not a writeback connector
 * @enable_overlay: Create or not overlay planes
 *
 * Returns:
 * The default castkms_config or an error. Call castkms_config_destroy() to free the
 * returned configuration. Display connectors start disconnected; userspace
 * attaches a monitor through the capture protocol.
 */
struct castkms_config *castkms_config_default_create(bool enable_cursor,
					       bool enable_writeback,
					       bool enable_overlay,
					       bool enable_plane_pipeline);

/**
 * castkms_config_default_max_outputs() - Maximum valid default output count
 * @enable_cursor: Create or not a cursor plane per output
 * @enable_writeback: Create or not a writeback connector per output
 * @enable_overlay: Create or not a shared overlay-plane pool
 *
 * The default topology creates feature-dependent DRM objects in addition to
 * each output's CRTC/encoder/connector tuple. This helper budgets those
 * derived objects against the DRM mask limits used by topology construction.
 *
 * Returns:
 * The largest output count accepted by castkms_config_default_create_outputs()
 * for the selected feature set.
 */
unsigned int castkms_config_default_max_outputs(bool enable_cursor,
						 bool enable_writeback,
						 bool enable_overlay);

/**
 * castkms_config_default_create_outputs() - Default device with N outputs
 * @enable_cursor: Create or not a cursor plane per output
 * @enable_writeback: Create or not a writeback connector per output
 * @enable_overlay: Create or not a shared overlay-plane pool
 * @enable_plane_pipeline: Enable the default plane color pipeline
 * @num_outputs: Number of CRTC/encoder/connector tuples to create
 *
 * Returns:
 * The default castkms_config or an error. Display connectors start
 * disconnected.
 */
struct castkms_config *castkms_config_default_create_outputs(bool enable_cursor,
					       bool enable_writeback,
					       bool enable_overlay,
					       bool enable_plane_pipeline,
					       unsigned int num_outputs);

/**
 * castkms_config_destroy() - Free a CASTKMS configuration
 * @config: castkms_config to free
 */
void castkms_config_destroy(struct castkms_config *config);

/**
 * castkms_config_clear_runtime_objects() - Clear instantiated DRM object bindings
 * @config: Configuration whose runtime bindings should be cleared
 *
 * Configuration nodes are reused when a configfs device is disabled and later
 * enabled again. Call this after failed device creation and before releasing an
 * instantiated device's managed resources.
 */
void castkms_config_clear_runtime_objects(struct castkms_config *config);

/**
 * castkms_config_get_device_name() - Return the name of the device
 * @config: Configuration to get the device name from
 *
 * Returns:
 * The device name. Only valid while @config is valid.
 */
static inline const char *
castkms_config_get_device_name(struct castkms_config *config)
{
	return config->dev_name;
}

/**
 * castkms_config_get_num_crtcs() - Return the number of CRTCs in the configuration
 * @config: Configuration to get the number of CRTCs from
 */
static inline size_t castkms_config_get_num_crtcs(struct castkms_config *config)
{
	return list_count_nodes(&config->crtcs);
}

/**
 * castkms_config_is_valid() - Validate a configuration
 * @config: Configuration to validate
 *
 * Returns:
 * Whether the configuration is valid or not.
 * For example, a configuration without primary planes is not valid.
 */
bool castkms_config_is_valid(const struct castkms_config *config);

/**
 * castkms_config_register_debugfs() - Register a debugfs file to show the device's
 * configuration
 * @castkms_device: Device to register
 */
void castkms_config_register_debugfs(struct castkms_device *castkms_device);

/**
 * castkms_config_create_plane() - Add a new plane configuration
 * @config: Configuration to add the plane to
 *
 * Returns:
 * The new plane configuration or an error. Call castkms_config_destroy_plane() to
 * free the returned plane configuration.
 */
struct castkms_config_plane *castkms_config_create_plane(struct castkms_config *config);

/**
 * castkms_config_destroy_plane() - Remove and free a plane configuration
 * @plane_cfg: Plane configuration to destroy
 */
void castkms_config_destroy_plane(struct castkms_config_plane *plane_cfg);

/**
 * castkms_config_plane_type() - Return the plane type
 * @plane_cfg: Plane to get the type from
 */
static inline enum drm_plane_type
castkms_config_plane_get_type(struct castkms_config_plane *plane_cfg)
{
	return plane_cfg->type;
}

/**
 * castkms_config_plane_set_type() - Set the plane type
 * @plane_cfg: Plane to set the type to
 * @type: New plane type
 */
static inline void
castkms_config_plane_set_type(struct castkms_config_plane *plane_cfg,
			   enum drm_plane_type type)
{
	plane_cfg->type = type;
}

/**
 * castkms_config_plane_get_default_pipeline() - Return if the plane will
 * be created with the default pipeline
 * @plane_cfg: Plane to get the information from
 */
static inline bool
castkms_config_plane_get_default_pipeline(struct castkms_config_plane *plane_cfg)
{
	return plane_cfg->default_pipeline;
}

/**
 * castkms_config_plane_set_default_pipeline() - Set if the plane will
 * be created with the default pipeline
 * @plane_cfg: Plane to configure the pipeline
 * @default_pipeline: New default pipeline value
 */
static inline void
castkms_config_plane_set_default_pipeline(struct castkms_config_plane *plane_cfg,
				       bool default_pipeline)
{
	plane_cfg->default_pipeline = default_pipeline;
}

/**
 * castkms_config_plane_attach_crtc - Attach a plane to a CRTC
 * @plane_cfg: Plane to attach
 * @crtc_cfg: CRTC to attach @plane_cfg to
 */
int __must_check castkms_config_plane_attach_crtc(struct castkms_config_plane *plane_cfg,
					       struct castkms_config_crtc *crtc_cfg);

/**
 * castkms_config_plane_detach_crtc - Detach a plane from a CRTC
 * @plane_cfg: Plane to detach
 * @crtc_cfg: CRTC to detach @plane_cfg from
 */
void castkms_config_plane_detach_crtc(struct castkms_config_plane *plane_cfg,
				   struct castkms_config_crtc *crtc_cfg);

/**
 * castkms_config_create_crtc() - Add a new CRTC configuration
 * @config: Configuration to add the CRTC to
 *
 * Returns:
 * The new CRTC configuration or an error. Call castkms_config_destroy_crtc() to
 * free the returned CRTC configuration.
 */
struct castkms_config_crtc *castkms_config_create_crtc(struct castkms_config *config);

/**
 * castkms_config_destroy_crtc() - Remove and free a CRTC configuration
 * @crtc_cfg: CRTC configuration to destroy
 */
void castkms_config_destroy_crtc(struct castkms_config_crtc *crtc_cfg);

/**
 * castkms_config_crtc_get_writeback() - If a writeback connector will be created
 * @crtc_cfg: CRTC with or without a writeback connector
 */
static inline bool
castkms_config_crtc_get_writeback(struct castkms_config_crtc *crtc_cfg)
{
	return crtc_cfg->writeback;
}

/**
 * castkms_config_crtc_set_writeback() - If a writeback connector will be created
 * @crtc_cfg: Target CRTC
 * @writeback: Enable or disable the writeback connector
 */
static inline void
castkms_config_crtc_set_writeback(struct castkms_config_crtc *crtc_cfg,
			       bool writeback)
{
	crtc_cfg->writeback = writeback;
}

/**
 * castkms_config_crtc_primary_plane() - Return the primary plane for a CRTC
 * @crtc_cfg: Target CRTC
 *
 * Note that, if multiple primary planes are found, the first one is returned.
 * In this case, the configuration will be invalid. See castkms_config_is_valid().
 *
 * Returns:
 * The primary plane or NULL if none is assigned yet.
 */
struct castkms_config_plane *
castkms_config_crtc_primary_plane(struct castkms_config_crtc *crtc_cfg);

/**
 * castkms_config_crtc_cursor_plane() - Return the cursor plane for a CRTC
 * @crtc_cfg: Target CRTC
 *
 * Note that, if multiple cursor planes are found, the first one is returned.
 * In this case, the configuration will be invalid. See castkms_config_is_valid().
 *
 * Returns:
 * The cursor plane or NULL if none is assigned yet.
 */
struct castkms_config_plane *
castkms_config_crtc_cursor_plane(struct castkms_config_crtc *crtc_cfg);

/**
 * castkms_config_create_encoder() - Add a new encoder configuration
 * @config: Configuration to add the encoder to
 *
 * Returns:
 * The new encoder configuration or an error. Call castkms_config_destroy_encoder()
 * to free the returned encoder configuration.
 */
struct castkms_config_encoder *castkms_config_create_encoder(struct castkms_config *config);

/**
 * castkms_config_destroy_encoder() - Remove and free a encoder configuration
 * @encoder_cfg: Encoder configuration to destroy
 */
void castkms_config_destroy_encoder(struct castkms_config_encoder *encoder_cfg);

/**
 * castkms_config_encoder_attach_crtc - Attach a encoder to a CRTC
 * @encoder_cfg: Encoder to attach
 * @crtc_cfg: CRTC to attach @encoder_cfg to
 */
int __must_check castkms_config_encoder_attach_crtc(struct castkms_config_encoder *encoder_cfg,
						 struct castkms_config_crtc *crtc_cfg);

/**
 * castkms_config_encoder_detach_crtc - Detach a encoder from a CRTC
 * @encoder_cfg: Encoder to detach
 * @crtc_cfg: CRTC to detach @encoder_cfg from
 */
void castkms_config_encoder_detach_crtc(struct castkms_config_encoder *encoder_cfg,
				     struct castkms_config_crtc *crtc_cfg);

/**
 * castkms_config_create_connector() - Add a new connector configuration
 * @config: Configuration to add the connector to
 *
 * Returns:
 * The new connector configuration or an error. Call
 * castkms_config_destroy_connector() to free the returned connector configuration.
 */
struct castkms_config_connector *castkms_config_create_connector(struct castkms_config *config);

/**
 * castkms_config_destroy_connector() - Remove and free a connector configuration
 * @connector_cfg: Connector configuration to destroy
 */
void castkms_config_destroy_connector(struct castkms_config_connector *connector_cfg);

/**
 * castkms_config_connector_attach_encoder - Attach a connector to an encoder
 * @connector_cfg: Connector to attach
 * @encoder_cfg: Encoder to attach @connector_cfg to
 */
int __must_check castkms_config_connector_attach_encoder(struct castkms_config_connector *connector_cfg,
						      struct castkms_config_encoder *encoder_cfg);

/**
 * castkms_config_connector_detach_encoder - Detach a connector from an encoder
 * @connector_cfg: Connector to detach
 * @encoder_cfg: Encoder to detach @connector_cfg from
 */
void castkms_config_connector_detach_encoder(struct castkms_config_connector *connector_cfg,
					  struct castkms_config_encoder *encoder_cfg);

/**
 * castkms_config_connector_get_status() - Return the status of the connector
 * @connector_cfg: Connector to get the status from
 */
static inline enum drm_connector_status
castkms_config_connector_get_status(struct castkms_config_connector *connector_cfg)
{
	return READ_ONCE(connector_cfg->status);
}

/**
 * castkms_config_connector_set_status() - Set the status of the connector
 * @connector_cfg: Connector to set the status to
 * @status: New connector status
 */
static inline void
castkms_config_connector_set_status(struct castkms_config_connector *connector_cfg,
				 enum drm_connector_status status)
{
	WRITE_ONCE(connector_cfg->status, status);
}

#endif /* _CASTKMS_CONFIG_H_ */
