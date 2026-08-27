// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kernel.h>

#include <drm/drm_atomic.h>
#include <drm/drm_auth.h>
#include <drm/drm_crtc.h>
#include <drm/drm_file.h>

#include <kunit/visibility.h>

#include "castkms_capture_owner.h"
#include "castkms_crtc.h"
#include "castkms_drv.h"
#include "castkms_output.h"

bool castkms_capture_owner_is_current(
	const struct drm_master *capture_owner,
	const struct drm_master *current_master)
{
	return current_master && capture_owner == current_master;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_owner_is_current);

static void castkms_capture_owner_copy_snapshot_state(
	const struct castkms_capture_owner_state *owners,
	const struct castkms_output *output,
	const struct drm_master *bound_master,
	struct castkms_capture_owner_snapshot *snapshot)
{
	lockdep_assert_held(&owners->lock);
	*snapshot = (struct castkms_capture_owner_snapshot) {
		.master_present = !!owners->master,
		.master_active = owners->master_active,
		.bound_master_current =
			castkms_capture_owner_is_current(bound_master,
						 owners->master),
		.cleanup_sequence = owners->cleanup_sequence,
	};
	if (output)
		snapshot->content_safe = !output->capture_owner_updating &&
			owners->master_active &&
			castkms_capture_owner_is_current(output->capture_owner,
						 owners->master);
}

void castkms_capture_owner_snapshot(
	struct drm_device *dev, const struct drm_master *bound_master,
	struct castkms_capture_owner_snapshot *snapshot)
{
	struct castkms_device *castkmsdev =
		drm_device_to_castkms_device(dev);
	struct castkms_capture_owner_state *owners = &castkmsdev->capture_owners;
	unsigned long flags;

	spin_lock_irqsave(&owners->lock, flags);
	castkms_capture_owner_copy_snapshot_state(
		owners, NULL, bound_master, snapshot);
	spin_unlock_irqrestore(&owners->lock, flags);
}

void castkms_capture_owner_take_output_snapshot(
	const struct castkms_output *output,
	const struct drm_master *bound_master,
	struct castkms_capture_owner_snapshot *snapshot)
{
	struct castkms_device *castkmsdev =
		drm_device_to_castkms_device(output->crtc.dev);
	struct castkms_capture_owner_state *owners = &castkmsdev->capture_owners;

	lockdep_assert_held(&output->lock);
	spin_lock(&owners->lock);
	castkms_capture_owner_copy_snapshot_state(
		owners, output, bound_master, snapshot);
	spin_unlock(&owners->lock);
}

struct drm_master *castkms_capture_owner_current_master_get(
	struct drm_device *dev)
{
	struct castkms_device *castkmsdev =
		drm_device_to_castkms_device(dev);
	struct castkms_capture_owner_state *owners = &castkmsdev->capture_owners;
	struct drm_master *master = NULL;
	unsigned long flags;

	spin_lock_irqsave(&owners->lock, flags);
	if (owners->master_active && owners->master)
		master = drm_master_get(owners->master);
	spin_unlock_irqrestore(&owners->lock, flags);

	return master;
}

bool castkms_capture_owner_is_active_current(
	struct drm_device *dev, const struct drm_master *capture_owner)
{
	struct castkms_capture_owner_snapshot snapshot;

	castkms_capture_owner_snapshot(dev, capture_owner, &snapshot);
	return snapshot.master_active && snapshot.bound_master_current;
}

bool castkms_capture_output_has_safe_content(
	const struct castkms_output *output)
{
	struct castkms_capture_owner_snapshot snapshot;

	castkms_capture_owner_take_output_snapshot(output, NULL, &snapshot);
	return snapshot.content_safe;
}

VISIBLE_IF_KUNIT bool
castkms_capture_blank_establishes_owner(bool old_state_exists,
					bool old_had_visible_planes,
					bool mode_changed,
					bool active_changed,
					bool background_changed)
{
	return !old_state_exists || old_had_visible_planes || mode_changed ||
	       active_changed || background_changed;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_blank_establishes_owner);

static void castkms_capture_owner_signal_transition(
	struct castkms_capture_owner_state *owners)
{
	lockdep_assert_held(&owners->lock);
	if (owners->shutdown)
		return;

	owners->transition++;
	schedule_work(&owners->work);
}

static void castkms_capture_owner_work_fn(struct work_struct *work)
{
	struct castkms_capture_owner_state *owners =
		container_of(work, struct castkms_capture_owner_state, work);
	u64 cleanup_sequence;
	u64 transition;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&owners->lock, flags);
		if (owners->shutdown) {
			spin_unlock_irqrestore(&owners->lock, flags);
			break;
		}
		transition = owners->transition;
		cleanup_sequence = owners->cleanup_sequence;
		spin_unlock_irqrestore(&owners->lock, flags);

		if (owners->ops && owners->ops->changed)
			owners->ops->changed(owners->data, cleanup_sequence);

		spin_lock_irqsave(&owners->lock, flags);
		if (owners->shutdown || owners->transition == transition) {
			spin_unlock_irqrestore(&owners->lock, flags);
			break;
		}
		spin_unlock_irqrestore(&owners->lock, flags);
		cond_resched();
	}
}

void castkms_capture_owner_device_init(
	struct castkms_device *castkmsdev,
	const struct castkms_capture_owner_ops *ops, void *data)
{
	struct castkms_capture_owner_state *owners = &castkmsdev->capture_owners;

	spin_lock_init(&owners->lock);
	INIT_WORK(&owners->work, castkms_capture_owner_work_fn);
	owners->ops = ops;
	owners->data = data;
}

void castkms_capture_owner_device_fini(struct castkms_device *castkmsdev)
{
	struct castkms_capture_owner_state *owners = &castkmsdev->capture_owners;
	struct drm_master *master;
	unsigned long flags;

	spin_lock_irqsave(&owners->lock, flags);
	owners->shutdown = true;
	spin_unlock_irqrestore(&owners->lock, flags);
	cancel_work_sync(&owners->work);

	spin_lock_irqsave(&owners->lock, flags);
	master = owners->master;
	owners->master = NULL;
	owners->master_file = NULL;
	owners->master_active = false;
	spin_unlock_irqrestore(&owners->lock, flags);
	if (master)
		drm_master_put(&master);
}

void castkms_capture_owner_publish(struct drm_atomic_commit *state)
{
	struct castkms_device *castkmsdev =
		drm_device_to_castkms_device(state->dev);
	struct castkms_capture_owner_state *owners = &castkmsdev->capture_owners;
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	unsigned long flags;
	bool changed = false;
	int i;

	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		struct castkms_crtc_state *castkms_state =
			to_castkms_crtc_state(crtc_state);
		struct castkms_output *output =
			drm_crtc_to_castkms_output(crtc);
		struct drm_master *new_owner = castkms_state->capture_owner;
		struct drm_master *old_owner;

		if (new_owner)
			drm_master_get(new_owner);
		spin_lock_irqsave(&output->lock, flags);
		old_owner = output->capture_owner;
		output->capture_owner = new_owner;
		output->capture_owner_updating = false;
		output->capture_owner_generation++;
		spin_unlock_irqrestore(&output->lock, flags);
		if (old_owner)
			drm_master_put(&old_owner);
		changed = true;
	}

	if (!changed)
		return;

	spin_lock_irqsave(&owners->lock, flags);
	castkms_capture_owner_signal_transition(owners);
	spin_unlock_irqrestore(&owners->lock, flags);
}

void castkms_capture_owner_master_set(
	struct drm_device *dev, struct drm_file *file_priv, bool from_open)
{
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_capture_owner_state *owners = &castkmsdev->capture_owners;
	struct drm_master *master = drm_file_get_master(file_priv);
	struct drm_master *old_master = NULL;
	unsigned long flags;

	(void)from_open;
	if (!master)
		return;

	spin_lock_irqsave(&owners->lock, flags);
	if (owners->shutdown) {
		spin_unlock_irqrestore(&owners->lock, flags);
		drm_master_put(&master);
		return;
	}

	if (owners->master == master) {
		owners->master_active = true;
		owners->master_file = file_priv;
	} else {
		if (owners->master_active)
			owners->cleanup_sequence++;
		old_master = owners->master;
		owners->master = master;
		master = NULL;
		owners->master_file = file_priv;
		owners->master_active = true;
	}
	castkms_capture_owner_signal_transition(owners);
	spin_unlock_irqrestore(&owners->lock, flags);

	if (old_master)
		drm_master_put(&old_master);
	if (master)
		drm_master_put(&master);
}

void castkms_capture_owner_master_drop(
	struct drm_device *dev, struct drm_file *file_priv)
{
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_capture_owner_state *owners = &castkmsdev->capture_owners;
	unsigned long flags;

	spin_lock_irqsave(&owners->lock, flags);
	if (!owners->shutdown && owners->master &&
	    owners->master_file == file_priv) {
		owners->master_active = false;
		owners->cleanup_sequence++;
		castkms_capture_owner_signal_transition(owners);
	}
	spin_unlock_irqrestore(&owners->lock, flags);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_owner_master_drop);

void castkms_capture_owner_file_close(
	struct drm_device *dev, struct drm_file *file_priv)
{
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_capture_owner_state *owners = &castkmsdev->capture_owners;
	unsigned long flags;

	spin_lock_irqsave(&owners->lock, flags);
	if (owners->master_file == file_priv) {
		bool was_active = owners->master_active;

		owners->master_file = NULL;
		owners->master_active = false;
		if (was_active)
			owners->cleanup_sequence++;
		castkms_capture_owner_signal_transition(owners);
	}
	spin_unlock_irqrestore(&owners->lock, flags);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_owner_file_close);
