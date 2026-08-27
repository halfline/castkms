// SPDX-License-Identifier: GPL-2.0+

#include <linux/device/faux.h>
#include <linux/hrtimer.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_eld.h>
#include <drm/drm_print.h>

#include <sound/control.h>
#include <sound/core.h>
#include <sound/info.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_drm_eld.h>
#include <sound/tlv.h>

#include "castkms_audio.h"
#include "castkms_config.h"
#include "castkms_connector.h"
#include "castkms_device.h"

struct castkms_audio_runtime {
	struct castkms_audio_output *output;
	struct snd_pcm_substream *substream;
	struct hrtimer timer;
	ktime_t period_duration;
	ktime_t base_time;
	u64 base_frames;
	unsigned int eld_generation;
	uint8_t eld[MAX_ELD_BYTES];
	bool running;
};

static const struct snd_pcm_hardware castkms_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_HAS_LINK_ESTIMATED_ATIME,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_32000 |
		 SNDRV_PCM_RATE_44100 |
		 SNDRV_PCM_RATE_48000,
	.rate_min = 32000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.period_bytes_min = 64 * 2 * 2,
	.period_bytes_max = 16384 * 2 * 2,
	.periods_min = 2,
	.periods_max = 32,
	.buffer_bytes_max = 65536 * 2 * 2,
};

/* --- frame position --- */

static u64 castkms_audio_frames_since_base(struct castkms_audio_runtime *rt,
					    ktime_t now, unsigned int rate)
{
	u64 elapsed_ns;

	elapsed_ns = ktime_to_ns(ktime_sub(now, rt->base_time));
	return mul_u64_u32_div(elapsed_ns, rate, NSEC_PER_SEC);
}

static u64 castkms_audio_total_frames(struct castkms_audio_runtime *rt,
				       ktime_t now, unsigned int rate)
{
	if (!READ_ONCE(rt->running))
		return rt->base_frames;

	return rt->base_frames +
	       castkms_audio_frames_since_base(rt, now, rate);
}

/* --- hrtimer --- */

static enum hrtimer_restart castkms_audio_timer(struct hrtimer *timer)
{
	struct castkms_audio_runtime *rt =
		container_of(timer, struct castkms_audio_runtime, timer);

	if (!READ_ONCE(rt->running))
		return HRTIMER_NORESTART;

	hrtimer_forward_now(timer, rt->period_duration);
	snd_pcm_period_elapsed(rt->substream);

	if (!READ_ONCE(rt->running))
		return HRTIMER_NORESTART;
	return HRTIMER_RESTART;
}

static void castkms_audio_timer_arm(struct castkms_audio_runtime *rt)
{
	WRITE_ONCE(rt->running, true);
	rt->base_time = ktime_get();
	hrtimer_start(&rt->timer, rt->period_duration, HRTIMER_MODE_REL_SOFT);
}

static void castkms_audio_timer_disarm(struct castkms_audio_runtime *rt,
				       unsigned int rate)
{
	ktime_t now = ktime_get();

	rt->base_frames = castkms_audio_total_frames(rt, now, rate);
	WRITE_ONCE(rt->running, false);
	hrtimer_try_to_cancel(&rt->timer);
}

/* --- PCM callbacks --- */

static int castkms_pcm_open(struct snd_pcm_substream *substream)
{
	struct castkms_audio_output *out = substream->private_data;
	struct castkms_audio_runtime *rt;
	int ret;

	mutex_lock(&out->lock);
	if (out->active_substream) {
		mutex_unlock(&out->lock);
		return -EBUSY;
	}

	rt = kzalloc(sizeof(*rt), GFP_KERNEL);
	if (!rt) {
		mutex_unlock(&out->lock);
		return -ENOMEM;
	}

	rt->output = out;
	rt->substream = substream;
	rt->eld_generation = out->generation;
	memcpy(rt->eld, out->eld, sizeof(rt->eld));
	hrtimer_setup(&rt->timer, castkms_audio_timer,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);

	substream->runtime->hw = castkms_pcm_hw;
	substream->runtime->private_data = rt;

	/* Constrain parameters to the monitor's capabilities when the
	 * ELD contains Short Audio Descriptors; fall back to the
	 * static hardware descriptor otherwise.
	 */
	ret = snd_pcm_hw_constraint_eld(substream->runtime, rt->eld);
	if (ret) {
		substream->runtime->private_data = NULL;
		kfree(rt);
		mutex_unlock(&out->lock);
		return ret;
	}

	out->active_substream = substream;
	mutex_unlock(&out->lock);

	return 0;
}

static int castkms_pcm_close(struct snd_pcm_substream *substream)
{
	struct castkms_audio_output *out = substream->private_data;
	struct castkms_audio_runtime *rt = substream->runtime->private_data;

	if (READ_ONCE(rt->running))
		castkms_audio_timer_disarm(rt, substream->runtime->rate);
	hrtimer_cancel(&rt->timer);

	mutex_lock(&out->lock);
	out->active_substream = NULL;
	mutex_unlock(&out->lock);

	kfree(rt);
	return 0;
}

static int castkms_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	struct castkms_audio_output *out = substream->private_data;
	struct castkms_audio_runtime *rt = substream->runtime->private_data;
	int ret = 0;

	mutex_lock(&out->lock);
	if (rt->eld_generation != out->generation)
		ret = -ENODEV;
	mutex_unlock(&out->lock);

	return ret;
}

static int castkms_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct castkms_audio_output *out = substream->private_data;
	struct castkms_audio_runtime *rt = substream->runtime->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	u64 period_ns;
	int ret = 0;

	mutex_lock(&out->lock);
	if (rt->eld_generation != out->generation)
		ret = -ENODEV;
	mutex_unlock(&out->lock);

	if (ret)
		return ret;

	period_ns = div_u64((u64)runtime->period_size * NSEC_PER_SEC,
			    runtime->rate);
	rt->period_duration = ns_to_ktime(period_ns);
	rt->base_frames = 0;
	rt->base_time = ktime_set(0, 0);

	return 0;
}

static int castkms_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct castkms_audio_runtime *rt = substream->runtime->private_data;
	unsigned int rate = substream->runtime->rate;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		rt->base_frames = 0;
		castkms_audio_timer_arm(rt);
		return 0;
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		castkms_audio_timer_arm(rt);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		castkms_audio_timer_disarm(rt, rate);
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t castkms_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct castkms_audio_runtime *rt = substream->runtime->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	u64 frames;

	frames = castkms_audio_total_frames(rt, ktime_get(), runtime->rate);

	return frames % runtime->buffer_size;
}

static int castkms_pcm_get_time_info(struct snd_pcm_substream *substream,
				     struct timespec64 *system_ts,
				     struct timespec64 *audio_ts,
				     struct snd_pcm_audio_tstamp_config *audio_tstamp_config,
				     struct snd_pcm_audio_tstamp_report *audio_tstamp_report)
{
	struct castkms_audio_runtime *rt = substream->runtime->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	ktime_t now = ktime_get();
	u64 frames;

	if (audio_tstamp_config->type_requested !=
	    SNDRV_PCM_AUDIO_TSTAMP_TYPE_LINK_ESTIMATED) {
		audio_tstamp_report->actual_type =
			SNDRV_PCM_AUDIO_TSTAMP_TYPE_DEFAULT;
		return 0;
	}

	*system_ts = ktime_to_timespec64(now);

	frames = castkms_audio_total_frames(rt, now, runtime->rate);
	*audio_ts = ns_to_timespec64(
		mul_u64_u32_div(frames, NSEC_PER_SEC, runtime->rate));

	audio_tstamp_report->valid = 1;
	audio_tstamp_report->actual_type =
		SNDRV_PCM_AUDIO_TSTAMP_TYPE_LINK_ESTIMATED;
	audio_tstamp_report->accuracy_report = 0;

	return 0;
}

static const struct snd_pcm_ops castkms_pcm_ops = {
	.open = castkms_pcm_open,
	.close = castkms_pcm_close,
	.hw_params = castkms_pcm_hw_params,
	.prepare = castkms_pcm_prepare,
	.trigger = castkms_pcm_trigger,
	.pointer = castkms_pcm_pointer,
	.get_time_info = castkms_pcm_get_time_info,
};

/* --- ELD control --- */

static int castkms_eld_ctl_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = MAX_ELD_BYTES;
	return 0;
}

static int castkms_eld_ctl_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct castkms_audio_output *out = kcontrol->private_data;

	mutex_lock(&out->lock);
	memcpy(ucontrol->value.bytes.data, out->eld, MAX_ELD_BYTES);
	mutex_unlock(&out->lock);

	return 0;
}

static const struct snd_kcontrol_new castkms_eld_ctl_template = {
	.access = SNDRV_CTL_ELEM_ACCESS_READ |
		  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.iface = SNDRV_CTL_ELEM_IFACE_PCM,
	.name = "ELD",
	.info = castkms_eld_ctl_info,
	.get = castkms_eld_ctl_get,
};

/* --- Output lookup --- */

static struct castkms_audio_output *
castkms_audio_find_output(struct castkms_device *castkmsdev,
			  struct drm_connector *connector)
{
	struct castkms_audio *audio = castkmsdev->audio;
	unsigned int i;

	if (!audio)
		return NULL;

	for (i = 0; i < audio->num_outputs; i++) {
		if (audio->outputs[i].connector == connector)
			return &audio->outputs[i];
	}

	return NULL;
}

/* --- Notification hooks --- */

void castkms_audio_notify_eld(struct castkms_device *castkmsdev,
			      struct drm_connector *connector)
{
	struct castkms_audio_output *out;
	struct snd_pcm_substream *substream = NULL;
	bool changed = false;
	bool available;

	out = castkms_audio_find_output(castkmsdev, connector);
	if (!out)
		return;

	mutex_lock(&connector->eld_mutex);
	available = drm_eld_size(connector->eld) > 0;

	mutex_lock(&out->lock);
	if (available != out->audio_available ||
	    memcmp(out->eld, connector->eld, MAX_ELD_BYTES)) {
		memcpy(out->eld, connector->eld, MAX_ELD_BYTES);
		out->audio_available = available;
		out->generation++;
		changed = true;
		if (out->active_substream &&
		    ((struct castkms_audio_runtime *)
		     out->active_substream->runtime->private_data)
		     ->eld_generation != out->generation)
			substream = out->active_substream;
	}
	mutex_unlock(&out->lock);
	mutex_unlock(&connector->eld_mutex);

	if (changed) {
		snd_ctl_notify_one(out->pcm->card,
				   SNDRV_CTL_EVENT_MASK_VALUE,
				   out->eld_ctl, 0);
		snd_jack_report(out->jack,
				available ? SND_JACK_AVOUT : 0);
	}

	if (substream)
		snd_pcm_stop_xrun(substream);
}

void castkms_audio_notify_disconnect(struct castkms_device *castkmsdev,
				     struct drm_connector *connector)
{
	struct castkms_audio_output *out;
	struct snd_pcm_substream *substream = NULL;

	out = castkms_audio_find_output(castkmsdev, connector);
	if (!out)
		return;

	mutex_lock(&out->lock);
	memset(out->eld, 0, MAX_ELD_BYTES);
	out->audio_available = false;
	out->generation++;
	substream = out->active_substream;
	mutex_unlock(&out->lock);

	snd_ctl_notify_one(out->pcm->card,
			   SNDRV_CTL_EVENT_MASK_VALUE,
			   out->eld_ctl, 0);
	snd_jack_report(out->jack, 0);

	if (substream)
		snd_pcm_stop_xrun(substream);
}

/* --- Per-output setup --- */

static int castkms_audio_output_init(struct castkms_audio *audio,
				     struct castkms_audio_output *out,
				     struct castkms_device *castkmsdev,
				     struct drm_connector *connector,
				     int index)
{
	struct snd_pcm *pcm;
	struct snd_kcontrol_new eld_ctl;
	char name[64];
	int ret;

	out->castkmsdev = castkmsdev;
	out->connector = connector;
	out->index = index;
	mutex_init(&out->lock);

	snprintf(name, sizeof(name), "Virtual HDMI %d", index);
	ret = snd_pcm_new(audio->card, name, index, 1, 0, &pcm);
	if (ret)
		return ret;

	pcm->private_data = out;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &castkms_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC,
				       NULL, 0, 0);
	ret = snd_pcm_add_chmap_ctls(pcm, SNDRV_PCM_STREAM_PLAYBACK,
				     snd_pcm_std_chmaps, 2, 0, NULL);
	if (ret)
		return ret;
	out->pcm = pcm;

	snprintf(name, sizeof(name), "HDMI/DP,pcm=%d", index);
	ret = snd_jack_new(audio->card, name, SND_JACK_AVOUT,
			   &out->jack, true, false);
	if (ret)
		return ret;

	eld_ctl = castkms_eld_ctl_template;
	eld_ctl.device = index;
	out->eld_ctl = snd_ctl_new1(&eld_ctl, out);
	if (!out->eld_ctl)
		return -ENOMEM;
	ret = snd_ctl_add(audio->card, out->eld_ctl);
	if (ret)
		return ret;

	return 0;
}

/* --- Device-level init/cleanup --- */

int castkms_audio_init(struct castkms_device *castkmsdev)
{
	struct castkms_audio *audio;
	struct snd_card *card;
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	unsigned int num_outputs = 0;
	unsigned int idx = 0;
	int ret;

	drm_connector_list_iter_begin(&castkmsdev->drm, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		if (connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK)
			num_outputs++;
	}
	drm_connector_list_iter_end(&iter);

	if (!num_outputs)
		return 0;

	audio = kzalloc(sizeof(*audio), GFP_KERNEL);
	if (!audio)
		return -ENOMEM;

	audio->outputs = kcalloc(num_outputs, sizeof(*audio->outputs),
				 GFP_KERNEL);
	if (!audio->outputs) {
		kfree(audio);
		return -ENOMEM;
	}
	audio->num_outputs = num_outputs;

	ret = snd_card_new(&castkmsdev->faux_dev->dev, -1, "CastKMS",
			   THIS_MODULE, 0, &card);
	if (ret) {
		kfree(audio->outputs);
		kfree(audio);
		return ret;
	}

	audio->card = card;
	strscpy(card->driver, "castkms");
	strscpy(card->shortname, "CastKMS Audio");
	snprintf(card->longname, sizeof(card->longname),
		 "CastKMS Virtual HDMI Audio %s",
		 dev_name(&castkmsdev->faux_dev->dev));

	castkmsdev->audio = audio;

	drm_connector_list_iter_begin(&castkmsdev->drm, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		struct castkms_connector *castkms_conn;

		if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
			continue;
		if (idx >= num_outputs)
			break;

		castkms_conn = drm_connector_to_castkms_connector(connector);

		ret = castkms_audio_output_init(audio, &audio->outputs[idx],
						castkmsdev, connector,
						castkms_conn->output_index);
		if (ret) {
			drm_connector_list_iter_end(&iter);
			goto err_card;
		}
		idx++;
	}
	drm_connector_list_iter_end(&iter);

	ret = snd_card_register(card);
	if (ret)
		goto err_card;

	return 0;

err_card:
	snd_card_free(card);
	castkmsdev->audio = NULL;
	kfree(audio->outputs);
	kfree(audio);
	return ret;
}

void castkms_audio_cleanup(struct castkms_device *castkmsdev)
{
	struct castkms_audio *audio = castkmsdev->audio;

	if (!audio)
		return;

	castkmsdev->audio = NULL;
	snd_card_free(audio->card);
	kfree(audio->outputs);
	kfree(audio);
}
