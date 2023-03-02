#include "ndi5-async-texture-filter.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(OBS_PLUGIN, OBS_PLUGIN_LANG)

// TODO Currently set to 60fps -- should be obs settings
// TODO Names! Names! Names!

const NDIlib_v5 *ndi5_lib = nullptr;

namespace NDI5AsyncFilter {

static const char *filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text(OBS_SETTING_UI_FILTER_NAME);
}

static bool filter_update_sender_name(obs_properties_t *, obs_property_t *,
				      void *data)
{
	auto filter = (struct filter *)data;

	obs_data_t *settings = obs_source_get_settings(filter->context);
	filter_update(filter, settings);
	obs_data_release(settings);

	return true;
}

static bool filter_debug_dev1(obs_properties_t *, obs_property_t *, void *data)
{
	auto filter = (struct filter *)data;

	return false;
}

static obs_properties_t *filter_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	auto props = obs_properties_create();

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_text(props, OBS_SETTING_UI_SENDER_NAME,
				obs_module_text(OBS_SETTING_UI_SENDER_NAME),
				OBS_TEXT_DEFAULT);

	obs_properties_add_button(props, OBS_SETTING_UI_BUTTON_TITLE,
				  obs_module_text(OBS_SETTING_UI_BUTTON_TITLE),
				  filter_update_sender_name);

	obs_properties_add_button(props, "dev_btn", "Dev #1",
				  filter_debug_dev1);

	return props;
}

static void filter_defaults(obs_data_t *defaults)
{
	obs_data_set_default_string(
		defaults, OBS_SETTING_UI_SENDER_NAME,
		obs_module_text(OBS_SETTING_DEFAULT_SENDER_NAME));
}

namespace NDI {

struct video_frame_desc {
	uint32_t width;
	uint32_t height;
	uint32_t y_stride;
	uint32_t u_stride;
	uint32_t v_stride;
	uint32_t uv_stride;
	video_format format;
	bool initialize;
	uint32_t framerate_D;
	uint32_t framerate_N;

	NDIlib_FourCC_video_type_e get_fourCC() const
	{
		switch (format) {
		case video_format::VIDEO_FORMAT_I420:
			return NDIlib_FourCC_type_I420;
		case video_format::VIDEO_FORMAT_NV12:
			return NDIlib_FourCC_type_NV12;
		default:
			return NDIlib_FourCC_type_NV12;
		}
	}
};

void update_video_frame_desc(void *data, const NDI::video_frame_desc &desc)
{
	auto filter = (struct filter *)data;

	if (!filter->first_run_update || desc.initialize) {
		filter->first_run_update = true;
		filter->ndi_video_frame.frame_rate_D =
			desc.framerate_D ? desc.framerate_D : 1000;
		filter->ndi_video_frame.frame_rate_N =
			desc.framerate_N ? desc.framerate_N : 60000;
		filter->ndi_video_frame.frame_format_type =
			NDIlib_frame_format_type_e::
				NDIlib_frame_format_type_progressive;

		filter->ndi_video_frame.FourCC = desc.get_fourCC();
	}

	// Update dimensions
	filter->ndi_video_frame.xres = desc.width;
	filter->ndi_video_frame.yres = desc.height;

	// Update stride values
	filter->y_stride = desc.y_stride;
	filter->uv_stride = desc.uv_stride;
	filter->ndi_video_frame.line_stride_in_bytes = desc.y_stride;
}

// Force NDI to process and release last frame
inline static void flush(void *data)
{
	auto filter = (struct filter *)data;
	if (!filter->ndi_sender)
		return;
	ndi5_lib->send_send_video_async_v2(filter->ndi_sender, NULL);
}

inline static void destroy_sender(void *data)
{
	auto filter = (struct filter *)data;

	// Destroy the NDI5 sender
	if (filter->sender_created)
		ndi5_lib->send_destroy(filter->ndi_sender);
}

int _async_counter;

inline static void create_sender(void *data)
{
	auto filter = (struct filter *)data;

	NDIlib_send_create_t desc;
	desc.p_ndi_name = filter->sender_name.c_str();
	desc.clock_video = false;

	filter->ndi_sender = ndi5_lib->send_create(&desc);

	if (!filter->ndi_sender) {
		error("could not create ndi sender");

		std::string tempName =
			filter->sender_name + std::to_string(_async_counter++);
		NDIlib_send_create_t desc2;
		desc2.p_ndi_name = tempName.c_str();
		desc2.clock_video = false;

		filter->ndi_sender = ndi5_lib->send_create(&desc2);

		if (!filter->ndi_sender) {
			error("could not create ndi sender #2");
			return;
		}
	}

	const NDIlib_source_t *ndi_info =
		ndi5_lib->send_get_source_name(filter->ndi_sender);

	if (!ndi_info) {
		blog(LOG_INFO, "Could not get NDI_INFO");
		return;
	}

	filter->sender_created = true;

	if (strcmp(filter->setting_sender_name, ndi_info->p_ndi_name) == 0) {
		return;
	}
}

static inline void send_buffer(void *data, int next_buffer_index)
{
	auto filter = (struct filter *)data;

	filter->ndi_video_frame.p_data =
		filter->ndi_frame_buffers[next_buffer_index];

	ndi5_lib->send_send_video_async_v2(filter->ndi_sender,
					   &filter->ndi_video_frame);

	filter->buffer_index = next_buffer_index;
}

} // namespace NDI

namespace Framebuffers {

inline static void destroy(void *data)
{
	auto filter = (struct filter *)data;
	std::ranges::for_each(filter->ndi_frame_buffers,
			      [](auto &ptr) { bfree(ptr); });
	filter->frame_allocated = false;
}

inline static size_t calculate_texture_size(video_format format, uint32_t width,
					    uint32_t height, uint32_t y_stride,
					    uint32_t uv_stride)
{
	// I420
	if (format == VIDEO_FORMAT_I420) {
		const size_t y_size = y_stride * height;
		const size_t uv_size = uv_stride * height / 2;
		return y_size + (2 * uv_size);
	}

	// NV12
	return width * height * 3 / 2;
}

inline static void create(void *data, video_format format, uint32_t width,
			  uint32_t height, uint32_t y_stride,
			  uint32_t uv_stride)
{
	auto filter = (struct filter *)data;

	if (filter->frame_allocated) {
		warn("NDI5 frame buffers destroyed unexpectedly");
		Framebuffers::destroy(filter);
	}

	size_t total_size = calculate_texture_size(format, width, height,
						   y_stride, uv_stride);

	// NDI Create the frame buffers
	std::ranges::for_each(
		filter->ndi_frame_buffers, [total_size](auto &ptr) {
			ptr = static_cast<uint8_t *>(bzalloc(total_size));
		});
	filter->frame_allocated = true;

	NDI::video_frame_desc desc = {};
	desc.format = format;
	desc.width = width;
	desc.height = height;
	desc.y_stride = y_stride;
	desc.uv_stride = uv_stride;

	NDI::update_video_frame_desc(filter, desc);
}

} // namespace Framebuffers

namespace Texture {

static void reset(void *data, video_format format, uint32_t width,
		  uint32_t height, uint32_t y_stride, uint32_t uv_stride)
{
	auto filter = (struct filter *)data;

	// Update Texture data
	filter->width = width;
	filter->height = height;

	// Clear NDI buffers and free last frame used
	NDI::flush(filter);

	// Flush our buffers and create new ones
	Framebuffers::destroy(filter);
	Framebuffers::create(filter, format, width, height, y_stride,
			     uv_stride);

	// Process NDI
	NDI::destroy_sender(filter);
	NDI::create_sender(filter);
}

// Returns a std::pair with previous and next buffer indexes
static std::pair<int, int> calculate_buffer_indexes(void *data)
{
	auto filter = (struct filter *)data;

	uint32_t prev_buffer_index = filter->buffer_index == 0
					     ? NDI_BUFFER_MAX
					     : filter->buffer_index - 1;

	uint32_t next_buffer_index = filter->buffer_index == NDI_BUFFER_MAX
					     ? 0
					     : filter->buffer_index + 1;

	return std::make_pair(prev_buffer_index, next_buffer_index);
}

} // namespace Texture

static void filter_render_i420(void *data, struct obs_source_frame *frame)
{
	auto filter = (struct filter *)data;

	if (filter->width != frame->width || filter->height != frame->height) {
		Texture::reset(filter, frame->format, frame->width,
			       frame->height, frame->linesize[0],
			       frame->linesize[1]);
	}

	auto [prev_buffer_index, next_buffer_index] =
		Texture::calculate_buffer_indexes(filter);

	const int width = frame->width;
	const size_t half_width = width / 2;
	const int height = frame->height;

	const int y_stride = frame->linesize[0];
	const int uv_stride = frame->linesize[1];

	const uint8_t *i420_data = frame->data[0];

	uint8_t *ndi_data = filter->ndi_frame_buffers[filter->buffer_index];

	for (int row = 0; row < height; row++) {
		// Y
		memcpy(ndi_data, i420_data, width);
		ndi_data += y_stride;
		i420_data += y_stride;

		if (row % 2 == 0) {
			// U
			memcpy(ndi_data, i420_data, half_width);
			ndi_data += uv_stride;
			i420_data += uv_stride;
			// V
			memcpy(ndi_data, i420_data, half_width);
			ndi_data += uv_stride;
			i420_data += uv_stride;
		}
	}

	NDI::send_buffer(filter, next_buffer_index);
}

static void filter_render_nv12(void *data, struct obs_source_frame *frame)
{
	auto filter = (struct filter *)data;

	if (filter->width != frame->width || filter->height != frame->height) {
		Texture::reset(filter, frame->format, frame->width,
			       frame->height, frame->linesize[0],
			       frame->linesize[1]);
	}

	auto [prev_buffer_index, next_buffer_index] =
		Texture::calculate_buffer_indexes(filter);

	const int width = frame->width;
	const size_t half_width = width / 2;
	const int height = frame->height;

	const int y_stride = frame->linesize[0];
	const int uv_stride = frame->linesize[1];
	const size_t half_uv_stride = uv_stride / 2;

	const uint8_t *nv12_data = frame->data[0];

	uint8_t *ndi_data = filter->ndi_frame_buffers[filter->buffer_index];

	for (int row = 0; row < height; row++) {
		// Copy Y plane
		memcpy(ndi_data, nv12_data, width);
		ndi_data += y_stride;
		nv12_data += y_stride;

		if (row % 2 == 0) {
			// Copy U plane
			memcpy(ndi_data, nv12_data, half_width);
			ndi_data += half_width;

			// Copy V plane
			memcpy(ndi_data, nv12_data + half_uv_stride,
			       half_width);
			ndi_data += half_uv_stride;
			nv12_data += uv_stride;
		}
	}

	NDI::send_buffer(filter, next_buffer_index);
}

static struct obs_source_frame *filter_video(void *data,
					     struct obs_source_frame *frame)
{
	if (!data)
		return frame;

	auto filter = (struct filter *)data;

	if (!filter->context)
		return frame;

	if (!filter->rendering)
		return frame;

	if (frame->width == 0 || frame->height == 0)
		return frame;

	// I420
	if (frame->format == VIDEO_FORMAT_I420) {
		filter_render_i420(filter, frame);
		return frame;
	}

	// NV12
	if (frame->format == VIDEO_FORMAT_NV12) {
		filter_render_nv12(filter, frame);
		return frame;
	}

	return frame;
}

static void filter_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);

	auto filter = (struct filter *)data;

	// If our names have changed, rebuild NDI
	/*
	if (strcmp(filter->setting_sender_name, filter->sender_name.c_str()) !=
	    0) {
		filter->rendering = false;

		// Change current sender
		filter->sender_name = std::string(filter->setting_sender_name);

		// Change this width to zero will force our texture
		// and NDI buffers to be rebuilt
		filter->width = 0;

		filter->rendering = true;
	}
	*/
}

//std::atomic<int> _counter;

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto filter = (struct filter *)bzalloc(sizeof(NDI5AsyncFilter::filter));

	// Baseline everything
	filter->buffer_index = 0;
	filter->width = 0;
	filter->height = 0;
	filter->frame_allocated = false;
	filter->sender_created = false;
	filter->rendering = false;

	// Setup the obs context
	filter->context = source;

	//std::string new_sender_name = std::string(OBS_SETTING_UI_SENDER_NAME) + "_" + std::to_string(_counter.fetch_add(1));

	// setup the ui setting
	filter->setting_sender_name =
		obs_data_get_string(settings, OBS_SETTING_UI_SENDER_NAME);

	/*
	std::string new_sender_name = std::string(filter->setting_sender_name) +
				      "_" +
				      std::to_string(_counter.fetch_add(1));
				      */

	// Copy it to our sendername
	filter->sender_name = std::string(filter->setting_sender_name);

	filter->rendering = true;

	// force an update
	filter_update(filter, settings);

	return filter;
}

static void filter_destroy(void *data)
{
	auto filter = (struct filter *)data;

	if (!filter)
		return;

	// Stop rendering
	filter->rendering = false;

	//obs_remove_main_render_callback(filter_render_callback, filter);

	// Flush NDI
	NDI::flush(filter);

	// Destroy sender
	if (filter->sender_created)
		ndi5_lib->send_destroy(filter->ndi_sender);

	// Destroy any framebuffers
	Framebuffers::destroy(filter);

	// ...
	filter->prev_target = nullptr;

	bfree(filter);
}

static void filter_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	auto filter = (struct filter *)data;

	if (!filter->context)
		return;

	obs_source_skip_video_filter(filter->context);
}

static void filter_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	auto filter = (struct filter *)data;
	filter->frame_count++;
}

// Writes a simple log entry to OBS
static inline void report_version()
{
#ifdef DEBUG
	info("you can haz maybe obs-ndi5-async-filter tooz (Version: %s)",
	     OBS_PLUGIN_VERSION_STRING);
#else
	info("obs-ndi5-async-filter [mrmahgu] - version %s",
	     OBS_PLUGIN_VERSION_STRING);
#endif
}

static inline bool report_buffer_count()
{
	if (NDI_BUFFER_COUNT < 3)
		return false;
	return true;
}

} // namespace NDI5AsyncFilter

std::unique_ptr<QLibrary> ndi5_qlibrary;

const NDIlib_v5 *load_ndi5_lib()
{

#ifdef _WIN32
	QFileInfo library_path(QDir(QString(qgetenv(NDILIB_REDIST_FOLDER)))
				       .absoluteFilePath(NDILIB_LIBRARY_NAME));
#elif __linux__
	// Check a bunch of possible locations and pick one
	QFileInfo library_path(
		QDir("/usr/local/lib").absoluteFilePath("libndi.so"));
#elif __APPLE__
	// Check a bunch of possible locations and pick one
#endif

	if (library_path.exists() && library_path.isFile()) {

		QString library_file_path = library_path.absoluteFilePath();

		ndi5_qlibrary =
			std::make_unique<QLibrary>(library_file_path, nullptr);

		if (ndi5_qlibrary->load()) {

			info("NDI runtime loaded");

			typedef const NDIlib_v5 *(*NDIlib_v5_load_)(void);

			NDIlib_v5_load_ library_load =
				(NDIlib_v5_load_)ndi5_qlibrary->resolve(
					"NDIlib_v5_load");

			if (library_load == nullptr) {
				error("NDI runtime 5 was not detected");
				return nullptr;
			}

			ndi5_qlibrary.reset();

			return library_load();
		}
	}
	error("NDI runtime could not be located.");
	return nullptr;
}

bool obs_module_load(void)
{
	auto filter_info = NDI5AsyncFilter::create_filter_info();

	obs_register_source(&filter_info);

	NDI5AsyncFilter::report_version();

	// Check our buffer count, if it is lower than expect, peace out
	// Developer check -- as only developer can change
	if (!NDI5AsyncFilter::report_buffer_count()) {
		error("critical error -- buffer count is too low");
		return false;
	}

	ndi5_lib = load_ndi5_lib();

	if (!ndi5_lib) {
		error("critical error");
		return false;
	}

	if (!ndi5_lib->initialize()) {
		error("NDI5 said nope -- your CPU is unsupported");
		return false;
	}

	info("NDI5 (%s) IS READY TO ROCK", ndi5_lib->version());

	return true;
}

void obs_module_unload()
{
	if (ndi5_lib)
		ndi5_lib->destroy();
}
