// SPDX-License-Identifier: GPL-2.0+
/*
 * vimc-streamer.c Virtual Media Controller Driver
 *
 * Copyright (C) 2018 Lucas A. M. Magalhães <lucmaga@gmail.com>
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/freezer.h>
#include <linux/kthread.h>

#include "vimc-streamer.h"

/*
 * vimc_streamer_pipeline_disable - Disable stream in all ved in stream
 *
 * @stream: the pointer to the stream structure with the pipeline to be
 *	    disabled.
 *
 * Calls s_stream to disable the stream in each entity of the pipeline
 *
 */
static void vimc_streamer_pipeline_disable(struct vimc_stream *stream)
{
	struct media_entity *entity;
	struct v4l2_subdev *sd;

	do {
		stream->pipe_size--;
		entity = stream->ved_pipeline[stream->pipe_size]->ent;
		entity = vimc_get_source_entity(entity);
		stream->ved_pipeline[stream->pipe_size] = NULL;
		/*
		 *  This may occur only if the streamer was not correctly
		 *  initialized.
		 */
		if (!entity)
			continue;

		if (!is_media_entity_v4l2_subdev(entity))
			continue;

		sd = media_entity_to_v4l2_subdev(entity);
		v4l2_subdev_call(sd, video, s_stream, 0);
	} while (stream->pipe_size);
}

/*
 * vimc_streamer_pipeline_init - initializes the stream structure
 *
 * @stream: the pointer to the stream structure to be initialized
 * @ved:    the pointer to the vimc entity initializing the stream
 *
 * Initializes the stream structure. Walks through the entity graph to
 * construct the pipeline used later on the streamer thread.
 * Calls s_stream to enable stream in all entities of the pipeline.
 */
static int vimc_streamer_pipeline_init(struct vimc_stream *stream,
				struct vimc_ent_device *ved)
{
	struct vimc_ent_device *source_ved = NULL;
	struct media_entity *entity;
	struct video_device *vdev;
	struct v4l2_subdev *sd;
	int ret = -EINVAL;

	stream->pipe_size = 0;
	stream->ved_pipeline[stream->pipe_size++] = ved;

	while (stream->pipe_size < VIMC_STREAMER_PIPELINE_MAX_SIZE) {
		if (!stream->ved_pipeline[stream->pipe_size-1])
			return 0;
		entity = stream->ved_pipeline[stream->pipe_size-1]->ent;
		entity = vimc_get_source_entity(entity);
		if (!entity)
			return 0;
		if (is_media_entity_v4l2_subdev(entity)) {
			sd = media_entity_to_v4l2_subdev(entity);
			ret = v4l2_subdev_call(sd, video, s_stream, 1);
			if (ret && ret != -ENOIOCTLCMD)
				break;
			source_ved = v4l2_get_subdevdata(sd);
		} else {
			vdev = container_of(entity,
					    struct video_device,
					    entity);
			source_ved = video_get_drvdata(vdev);
		}

		stream->ved_pipeline[stream->pipe_size++] = source_ved;
		source_ved = NULL;
	}

	/*
	 * If an error occur during initialization or the pipeline gets longer
	 * than VIMC_STREAMER_PIPELINE_MAX_SIZE the stream is disabled and
	 * return the error code.
	 */
	vimc_streamer_pipeline_disable(stream);
	return ret;
}

static int vimc_streamer_thread(void *data)
{
	struct vimc_stream *stream = data;
	int i;

	set_freezable();
	set_current_state(TASK_UNINTERRUPTIBLE);

	for (;;) {
		try_to_freeze();
		if (kthread_should_stop())
			break;

		for (i = stream->pipe_size - 1; i >= 0; i--) {
			stream->frame = stream->ved_pipeline[i]->process_frame(
					stream->ved_pipeline[i],
					stream->frame);
			if (!stream->frame)
				break;
			if (IS_ERR(stream->frame))
				break;
		}
		//wait for 60hz
		schedule_timeout(HZ / 60);
	}

	return 0;
}

int vimc_streamer_s_stream(struct vimc_stream *stream,
			   struct vimc_ent_device *ved,
			   int enable)
{
	int ret;

	if (!stream || !ved)
		return -EINVAL;

	if (enable) {
		if (stream->kthread)
			return 0;

		ret = vimc_streamer_pipeline_init(stream, ved);
		if (ret)
			return ret;

		stream->kthread = kthread_run(vimc_streamer_thread, stream,
					      "vimc-streamer thread");

		if (IS_ERR(stream->kthread))
			return PTR_ERR(stream->kthread);

	} else {
		if (!stream->kthread)
			return 0;

		ret = kthread_stop(stream->kthread);
		if (ret)
			return ret;

		stream->kthread = NULL;

		vimc_streamer_pipeline_disable(stream);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(vimc_streamer_s_stream);

MODULE_DESCRIPTION("Virtual Media Controller Driver (VIMC) Streamer");
MODULE_AUTHOR("Lucas A. M. Magalhães <lucmaga@gmail.com>");
MODULE_LICENSE("GPL");
