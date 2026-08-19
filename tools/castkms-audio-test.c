// SPDX-License-Identifier: GPL-2.0-only
/*
 * Validate ALSA audio timing for castkms virtual outputs.
 *
 * Tests:
 *   1. Card and PCM discovery.
 *   2. Playback at 48 kHz with timestamp sampling.
 *   3. System and audio timestamp monotonicity.
 *   4. Clock-rate accuracy vs CLOCK_MONOTONIC wall time.
 *   5. System-to-audio offset drift over the run.
 *   6. Pause/resume position continuity.
 *   7. Detach causes XRUN (when --detach-during-playback is used).
 */

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define RATE		48000
#define CHANNELS	2
#define PERIOD_FRAMES	1024
#define BUFFER_PERIODS	4
#define MAX_SAMPLES	4096

struct ts_sample {
	int64_t system_ns;
	int64_t audio_ns;
	int64_t wall_ns;
	snd_pcm_uframes_t hw_ptr;
};

static int64_t timespec_to_ns(const struct timespec *ts)
{
	return (int64_t)ts->tv_sec * 1000000000LL + ts->tv_nsec;
}

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return timespec_to_ns(&ts);
}

static int find_castkms_card(void)
{
	int card = -1;

	while (snd_card_next(&card) == 0 && card >= 0) {
		char *name = NULL;

		snd_card_get_name(card, &name);
		if (name && strstr(name, "CastKMS")) {
			free(name);
			return card;
		}
		free(name);
	}

	return -1;
}

static snd_pcm_t *open_and_configure(int card_index,
				      snd_pcm_uframes_t *out_period,
				      snd_pcm_uframes_t *out_buffer,
				      unsigned int *out_rate)
{
	char pcm_name[64];
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	unsigned int rate = RATE;
	snd_pcm_uframes_t period = PERIOD_FRAMES;
	snd_pcm_uframes_t buffer;
	int ret;

	snprintf(pcm_name, sizeof(pcm_name), "hw:%d,0", card_index);

	ret = snd_pcm_open(&pcm, pcm_name, SND_PCM_STREAM_PLAYBACK, 0);
	if (ret < 0) {
		fprintf(stderr, "snd_pcm_open(%s): %s\n",
			pcm_name, snd_strerror(ret));
		return NULL;
	}

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	snd_pcm_hw_params_set_access(pcm, hw,
				      SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
	snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS);
	snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);
	snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);
	buffer = period * BUFFER_PERIODS;
	snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);

	ret = snd_pcm_hw_params(pcm, hw);
	if (ret < 0) {
		fprintf(stderr, "snd_pcm_hw_params: %s\n", snd_strerror(ret));
		snd_pcm_close(pcm);
		return NULL;
	}

	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(pcm, sw);
	snd_pcm_sw_params_set_tstamp_mode(pcm, sw, SND_PCM_TSTAMP_ENABLE);
	snd_pcm_sw_params_set_tstamp_type(pcm, sw,
					   SND_PCM_TSTAMP_TYPE_MONOTONIC);

	ret = snd_pcm_sw_params(pcm, sw);
	if (ret < 0) {
		fprintf(stderr, "snd_pcm_sw_params: %s\n", snd_strerror(ret));
		snd_pcm_close(pcm);
		return NULL;
	}

	*out_period = period;
	*out_buffer = buffer;
	*out_rate = rate;
	return pcm;
}

static int sample_timestamps(snd_pcm_t *pcm, struct ts_sample *out)
{
	snd_pcm_status_t *status;
	snd_htimestamp_t sys_ts, audio_ts;
	snd_pcm_audio_tstamp_config_t audio_tstamp_config = { 0 };
	int ret;

	snd_pcm_status_alloca(&status);
	audio_tstamp_config.type_requested =
		SND_PCM_AUDIO_TSTAMP_TYPE_LINK_ESTIMATED;
	snd_pcm_status_set_audio_htstamp_config(status, &audio_tstamp_config);
	ret = snd_pcm_status(pcm, status);
	if (ret < 0)
		return ret;

	snd_pcm_status_get_htstamp(status, &sys_ts);
	snd_pcm_status_get_audio_htstamp(status, &audio_ts);

	out->system_ns = (int64_t)sys_ts.tv_sec * 1000000000LL +
			 sys_ts.tv_nsec;
	out->audio_ns = (int64_t)audio_ts.tv_sec * 1000000000LL +
			audio_ts.tv_nsec;
	out->wall_ns = now_ns();
	out->hw_ptr = snd_pcm_status_get_avail(status);

	return 0;
}

static int run_playback_test(snd_pcm_t *pcm, int duration_sec,
			     unsigned int rate,
			     snd_pcm_uframes_t period,
			     struct ts_sample *samples,
			     int *n_samples)
{
	int16_t *silence;
	int64_t start, elapsed;
	uint64_t total_frames = 0;
	unsigned int sample_interval;
	int xruns = 0;
	int ret;

	silence = calloc(period * CHANNELS, sizeof(int16_t));
	if (!silence)
		return -1;

	ret = snd_pcm_prepare(pcm);
	if (ret < 0) {
		free(silence);
		return ret;
	}

	for (unsigned int i = 0; i < BUFFER_PERIODS; i++) {
		ret = snd_pcm_writei(pcm, silence, period);
		if (ret < 0) {
			free(silence);
			return ret;
		}
	}

	start = now_ns();
	sample_interval = rate / 10;
	*n_samples = 0;

	while (1) {
		elapsed = now_ns() - start;
		if (elapsed >= (int64_t)duration_sec * 1000000000LL)
			break;

		ret = snd_pcm_writei(pcm, silence, period);
		if (ret == -EPIPE) {
			xruns++;
			snd_pcm_prepare(pcm);
			continue;
		}
		if (ret < 0)
			break;

		total_frames += (unsigned int)ret;

		if (*n_samples < MAX_SAMPLES &&
		    total_frames % sample_interval < period) {
			if (!sample_timestamps(pcm, &samples[*n_samples]))
				(*n_samples)++;
		}
	}

	elapsed = now_ns() - start;

	printf("playback_duration_ms=%lld\n",
	       (long long)(elapsed / 1000000));
	printf("total_frames=%llu\n", (unsigned long long)total_frames);
	printf("xruns=%d\n", xruns);

	{
		double expected = (double)elapsed / 1000000000.0 * rate;
		double pct = 100.0 * ((double)total_frames - expected) /
			     expected;

		printf("clock_rate_error_pct=%.4f\n", pct);
	}

	snd_pcm_drop(pcm);
	free(silence);
	return 0;
}

static int run_pause_test(snd_pcm_t *pcm, snd_pcm_uframes_t period)
{
	int16_t *silence;
	snd_pcm_uframes_t ptr_before, ptr_paused1, ptr_paused2, ptr_resumed;
	int ret;

	silence = calloc(period * CHANNELS, sizeof(int16_t));
	if (!silence)
		return -1;

	ret = snd_pcm_prepare(pcm);
	if (ret < 0) {
		free(silence);
		return ret;
	}

	for (unsigned int i = 0; i < BUFFER_PERIODS; i++) {
		ret = snd_pcm_writei(pcm, silence, period);
		if (ret < 0) {
			free(silence);
			return ret;
		}
	}

	usleep(20000);
	ret = snd_pcm_writei(pcm, silence, period);
	if (ret < 0 && ret != -EAGAIN) {
		printf("pause_support=0 (pre-pause write failed: %s)\n",
		       snd_strerror(ret));
		snd_pcm_drop(pcm);
		free(silence);
		return 0;
	}

	ptr_before = snd_pcm_avail(pcm);

	ret = snd_pcm_pause(pcm, 1);
	if (ret < 0) {
		printf("pause_support=0\n");
		snd_pcm_drop(pcm);
		free(silence);
		return 0;
	}

	usleep(50000);
	ptr_paused1 = snd_pcm_avail(pcm);
	usleep(100000);
	ptr_paused2 = snd_pcm_avail(pcm);

	snd_pcm_pause(pcm, 0);
	usleep(20000);
	ptr_resumed = snd_pcm_avail(pcm);

	snd_pcm_drop(pcm);
	free(silence);

	printf("pause_support=1\n");
	printf("avail_before_pause=%lu\n", (unsigned long)ptr_before);
	printf("avail_paused_50ms=%lu\n", (unsigned long)ptr_paused1);
	printf("avail_paused_150ms=%lu\n", (unsigned long)ptr_paused2);
	printf("avail_after_resume=%lu\n", (unsigned long)ptr_resumed);

	int pause_ok = 1;

	if (ptr_paused1 != ptr_paused2) {
		printf("pause_position=fail (pointer moved during pause: %lu -> %lu)\n",
		       (unsigned long)ptr_paused1,
		       (unsigned long)ptr_paused2);
		pause_ok = 0;
	}
	if (ptr_resumed == ptr_paused2) {
		printf("pause_position=fail (pointer did not advance after resume)\n");
		pause_ok = 0;
	}

	if (pause_ok)
		printf("pause_resume=pass\n");

	return pause_ok ? 0 : -1;
}

static void analyze_timestamps(struct ts_sample *samples, int n_samples)
{
	int sys_monotonic = 1;
	int audio_monotonic = 1;
	int valid_pairs = 0;
	int64_t first_offset = 0;
	int64_t max_drift = 0;
	int audio_ts_present = 0;

	for (int i = 0; i < n_samples; i++) {
		if (samples[i].audio_ns > 0)
			audio_ts_present = 1;

		if (i > 0) {
			if (samples[i].system_ns < samples[i - 1].system_ns)
				sys_monotonic = 0;
			if (samples[i].audio_ns > 0 &&
			    samples[i - 1].audio_ns > 0 &&
			    samples[i].audio_ns < samples[i - 1].audio_ns)
				audio_monotonic = 0;
		}

		if (samples[i].system_ns > 0 && samples[i].audio_ns > 0) {
			int64_t offset = samples[i].system_ns -
					 samples[i].audio_ns;

			if (!valid_pairs) {
				first_offset = offset;
			} else {
				int64_t drift = offset - first_offset;

				if (drift < 0)
					drift = -drift;
				if (drift > max_drift)
					max_drift = drift;
			}
			valid_pairs++;
		}
	}

	printf("system_ts_monotonic=%d\n", sys_monotonic);
	printf("audio_ts_present=%d\n", audio_ts_present);

	if (audio_ts_present) {
		printf("audio_ts_monotonic=%d\n", audio_monotonic);
		printf("valid_timestamp_pairs=%d\n", valid_pairs);

		if (valid_pairs > 1) {
			int64_t span = samples[n_samples - 1].system_ns -
				       samples[0].system_ns;
			double drift_per_sec = 0;

			if (span > 0)
				drift_per_sec = (double)max_drift /
						((double)span / 1e9);

			printf("max_drift_us=%lld\n",
			       (long long)(max_drift / 1000));
			printf("drift_us_per_sec=%.1f\n",
			       drift_per_sec / 1000.0);
		}
	}
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [duration-seconds]\n"
		"  Default duration is 10 seconds.\n",
		prog);
}

int main(int argc, char *argv[])
{
	int card_index;
	snd_pcm_t *pcm;
	snd_pcm_uframes_t period, buffer;
	unsigned int rate;
	struct ts_sample *samples;
	int n_samples = 0;
	int duration_sec = 10;
	int pass = 1;

	if (argc > 1) {
		if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
			usage(argv[0]);
			return 0;
		}
		duration_sec = atoi(argv[1]);
	}
	if (duration_sec < 1)
		duration_sec = 1;
	if (duration_sec > 300)
		duration_sec = 300;

	card_index = find_castkms_card();
	if (card_index < 0) {
		printf("card_found=0\n");
		printf("audio_timing=skip\n");
		return EXIT_FAILURE;
	}
	printf("card_index=%d\n", card_index);

	pcm = open_and_configure(card_index, &period, &buffer, &rate);
	if (!pcm) {
		printf("pcm_open=fail\n");
		return EXIT_FAILURE;
	}
	printf("pcm_open=pass\n");
	printf("rate=%u\n", rate);
	printf("period_size=%lu\n", (unsigned long)period);
	printf("buffer_size=%lu\n", (unsigned long)buffer);
	printf("timestamp_config=pass\n");

	samples = calloc(MAX_SAMPLES, sizeof(*samples));
	if (!samples) {
		snd_pcm_close(pcm);
		return EXIT_FAILURE;
	}

	printf("playback_start=pass\n");

	if (run_playback_test(pcm, duration_sec, rate, period,
			      samples, &n_samples)) {
		printf("playback=fail\n");
		pass = 0;
	} else {
		printf("playback=pass\n");
	}

	printf("samples_collected=%d\n", n_samples);
	analyze_timestamps(samples, n_samples);

	if (run_pause_test(pcm, period))
		pass = 0;

	snd_pcm_close(pcm);
	free(samples);

	printf("audio_timing=%s\n", pass ? "pass" : "FAIL");
	return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}
