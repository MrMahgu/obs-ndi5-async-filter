#pragma once

#include <memory>
#include <atomic>
#include <string>
#include <ranges>
#include <unordered_map>

#include <obs-module.h>
#include <graphics/graphics.h>

#include <QString>
#include <QLibrary>
#include <QFileInfo>
#include <QDir>

#include "inc/Processing.NDI.Lib.h"

/* clang-format off */

#define OBS_PLUGIN                         "obs-ndi5-async-filter"
#define OBS_PLUGIN_                        "obs_ndi5_async_filter"

#define OBS_PLUGIN_VERSION_MAJOR           0
#define OBS_PLUGIN_VERSION_MINOR           0
#define OBS_PLUGIN_VERSION_RELEASE         1
#define OBS_PLUGIN_VERSION_STRING          "0.0.1"

#define OBS_PLUGIN_LANG                    "en-US"

#define OBS_SETTING_UI_FILTER_NAME         "mahgu.ndi5async.ui.filter_title"
#define OBS_SETTING_UI_SENDER_NAME         "mahgu.ndi5async.ui.sender_name"
#define OBS_SETTING_UI_BUTTON_TITLE        "mahgu.ndi5async.ui.button_title"
#define OBS_SETTING_DEFAULT_SENDER_NAME    "mahgu.ndi5async.default.sender_name"

/* clang-format on */

constexpr int NDI_BUFFER_COUNT = 4; // CURRENTLY NEEDS TO BE MIN 3
constexpr int NDI_BUFFER_MAX = NDI_BUFFER_COUNT - 1;

#define obs_log(level, format, ...) \
	blog(level, "[obs-ndi5-async-filter] " format, ##__VA_ARGS__)

#define error(format, ...) obs_log(LOG_ERROR, format, ##__VA_ARGS__)
#define warn(format, ...) obs_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) obs_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) obs_log(LOG_DEBUG, format, ##__VA_ARGS__)

namespace NDI5AsyncFilter {

static const char *filter_get_name(void *unused);
static obs_properties_t *filter_properties(void *unused);

static void filter_defaults(obs_data_t *defaults);
static void *filter_create(obs_data_t *settings, obs_source_t *source);
static void filter_destroy(void *data);
static void filter_update(void *data, obs_data_t *settings);
static void filter_video_tick(void *data, float seconds);
static struct obs_source_frame *filter_video(void *data,
					     struct obs_source_frame *frame);

struct filter {
	obs_source_t *context;

	gs_texture_t *prev_target;

	uint8_t *ndi_frame_buffers[NDI_BUFFER_COUNT];

	NDIlib_video_frame_v2_t ndi_video_frame;

	NDIlib_send_instance_t ndi_sender;

	bool sender_created;
	bool frame_allocated;
	bool first_run_update;
	bool rendering;

	uint32_t width;
	uint32_t height;

	uint32_t linesize;
	uint32_t buffer_index;
	uint32_t frame_count;

	uint32_t y_stride;  // I420, NV12
	uint32_t u_stride;  // I420
	uint32_t v_stride;  // I420
	uint32_t uv_stride; // NV12

	const char *setting_sender_name; // realtime setting

	std::string sender_name; // ndi sender name
};

struct obs_source_info create_filter_info()
{
	struct obs_source_info filter_info = {};

	filter_info.id = OBS_PLUGIN_;
	filter_info.type = OBS_SOURCE_TYPE_FILTER;
	filter_info.output_flags = OBS_SOURCE_ASYNC_VIDEO;

	filter_info.get_name = filter_get_name;
	filter_info.get_properties = filter_properties;
	filter_info.get_defaults = filter_defaults;
	filter_info.create = filter_create;
	filter_info.destroy = filter_destroy;
	filter_info.update = filter_update;
	filter_info.video_tick = filter_video_tick;
	filter_info.filter_video = filter_video;

	return filter_info;
};

} // namespace NDI5AsyncFilter
