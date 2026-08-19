/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_AUDIO_H_
#define _CASTKMS_AUDIO_H_

#include <linux/mutex.h>

struct castkms_device;
struct drm_connector;

#if IS_ENABLED(CONFIG_SND)

#include <drm/drm_connector.h>
#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>

struct castkms_audio_output {
	struct castkms_device *castkmsdev;
	struct drm_connector *connector;
	int index;

	struct snd_pcm *pcm;
	struct snd_jack *jack;
	struct snd_kcontrol *eld_ctl;

	struct mutex lock;
	uint8_t eld[MAX_ELD_BYTES];
	bool audio_available;
	unsigned int generation;
	struct snd_pcm_substream *active_substream;
};

struct castkms_audio {
	struct snd_card *card;
	unsigned int num_outputs;
	struct castkms_audio_output *outputs;
};

int castkms_audio_init(struct castkms_device *castkmsdev);
void castkms_audio_cleanup(struct castkms_device *castkmsdev);
void castkms_audio_notify_eld(struct castkms_device *castkmsdev,
			      struct drm_connector *connector);
void castkms_audio_notify_disconnect(struct castkms_device *castkmsdev,
				     struct drm_connector *connector);

#else /* !CONFIG_SND */

struct castkms_audio;

static inline int
castkms_audio_init(struct castkms_device *castkmsdev)
{
	return 0;
}

static inline void
castkms_audio_cleanup(struct castkms_device *castkmsdev) { }

static inline void
castkms_audio_notify_eld(struct castkms_device *castkmsdev,
			 struct drm_connector *connector) { }

static inline void
castkms_audio_notify_disconnect(struct castkms_device *castkmsdev,
				struct drm_connector *connector) { }

#endif /* CONFIG_SND */
#endif /* _CASTKMS_AUDIO_H_ */
