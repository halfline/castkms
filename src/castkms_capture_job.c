// SPDX-License-Identifier: GPL-2.0-only

#include <linux/slab.h>
#include <linux/workqueue.h>

#include "castkms_capture.h"
#include "castkms_composer.h"
#include "castkms_output.h"
#include "castkms_output_buffer.h"
#include "castkms_snapshot.h"

struct castkms_capture_job {
	struct work_struct work;
	struct castkms_output *output;
	struct castkms_frame_snapshot *snapshot;
	struct castkms_capture_buffer *buffer;
};

static void castkms_capture_job_worker(struct work_struct *work)
{
	struct castkms_capture_job *job =
		container_of(work, struct castkms_capture_job, work);
	const struct castkms_output_buffer *destination;
	int ret;

	destination = castkms_capture_buffer_output(job->buffer);
	ret = castkms_frame_snapshot_wait_for_sources(job->snapshot);
	if (!ret)
		ret = castkms_compose_frame(&job->snapshot->frame, destination);
	castkms_capture_buffer_set_damage(job->buffer,
					  &job->snapshot->frame.damage,
					  job->snapshot->frame.full_damage);
	castkms_capture_complete_frame(job->output, job->buffer, ret);
	castkms_frame_snapshot_put(job->snapshot);
	kfree(job);
}

void castkms_capture_queue_job(struct castkms_output *output,
			       struct castkms_capture_buffer *buffer,
			       struct castkms_frame_snapshot *snapshot)
{
	struct castkms_capture_job *job;

	job = kzalloc_obj(*job);
	if (!job) {
		castkms_capture_complete_frame(output, buffer, -ENOMEM);
		castkms_frame_snapshot_put(snapshot);
		return;
	}

	INIT_WORK(&job->work, castkms_capture_job_worker);
	job->output = output;
	job->snapshot = snapshot;
	job->buffer = buffer;
	if (!queue_work(output->capture_workq, &job->work)) {
		castkms_capture_complete_frame(output, buffer, -ENODEV);
		castkms_frame_snapshot_put(snapshot);
		kfree(job);
	}
}
