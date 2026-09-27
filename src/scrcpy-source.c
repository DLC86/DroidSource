#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include "scrcpy-source.h"
#include "scrcpy-adb.h"
#include "scrcpy-process.h"
#include "scrcpy-reader.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/dstr.h>

#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define SCRCPY_EXE_NAME "scrcpy.exe"

struct scrcpy_src {
	obs_source_t *source;
	scrcpy_proc_t proc;
	bool proc_alive;
	scrcpy_reader_t *reader;
	uint16_t port;

	char *serial;
	char *video_source;
	int camera_id;
	char *camera_size;
	int camera_fps;
	float camera_zoom;
	bool camera_torch;
	int camera_iso;
	int camera_shutter_us;
	float camera_focus_distance;
	char *camera_awb_mode;
	int max_size;
	int bitrate_kbps;
	char *codec;
};

static const char *src_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ScrcpySource");
}

static bool pick_ephemeral_port(uint16_t *out)
{
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;
#endif
	int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return false;

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
#ifdef _WIN32
		closesocket(fd);
#else
		close(fd);
#endif
		return false;
	}

	struct sockaddr_in bound = {0};
	socklen_t len = sizeof(bound);
	if (getsockname(fd, (struct sockaddr *)&bound, &len) != 0) {
#ifdef _WIN32
		closesocket(fd);
#else
		close(fd);
#endif
		return false;
	}
	*out = ntohs(bound.sin_port);
#ifdef _WIN32
	closesocket(fd);
#else
	close(fd);
#endif
	return true;
}

static void start_scrcpy(struct scrcpy_src *ctx, obs_data_t *settings)
{
	uint16_t port = 0;
	if (!pick_ephemeral_port(&port)) {
		obs_log(LOG_ERROR, "scrcpy-source: could not pick port");
		return;
	}
	ctx->port = port;

	char *bin_dir = get_bin_dir();
	if (!bin_dir) {
		obs_log(LOG_ERROR, "scrcpy-source: cannot locate scrcpy binary dir "
				   "(set SCRCPY_OBS_BIN_DIR)");
		return;
	}

	char *exe_path = path_join(bin_dir, SCRCPY_EXE_NAME);
	char bitrate_arg[64];
	snprintf(bitrate_arg, sizeof(bitrate_arg), "--video-bit-rate=%dK", ctx->bitrate_kbps);

	char max_size_arg[64] = {0};
	if (ctx->max_size > 0)
		snprintf(max_size_arg, sizeof(max_size_arg), "--max-size=%d", ctx->max_size);

	char codec_arg[64] = {0};
	if (ctx->codec && *ctx->codec)
		snprintf(codec_arg, sizeof(codec_arg), "--video-codec=%s", ctx->codec);

	char source_arg[64] = {0};
	if (ctx->video_source && *ctx->video_source)
		snprintf(source_arg, sizeof(source_arg), "--video-source=%s", ctx->video_source);

	char camera_arg[64] = {0};
	char camera_size_arg[64] = {0};
	char camera_fps_arg[64] = {0};
	char camera_zoom_arg[64] = {0};
	char camera_torch_arg[64] = {0};
	char camera_iso_arg[64] = {0};
	char camera_shutter_arg[64] = {0};
	char camera_focus_arg[64] = {0};
	char camera_awb_arg[64] = {0};
	if (ctx->video_source && strcmp(ctx->video_source, "camera") == 0) {
		snprintf(camera_arg, sizeof(camera_arg), "--camera-id=%d", ctx->camera_id);
		if (ctx->camera_size && *ctx->camera_size)
			snprintf(camera_size_arg, sizeof(camera_size_arg), "--camera-size=%s", ctx->camera_size);
		if (ctx->camera_fps > 0)
			snprintf(camera_fps_arg, sizeof(camera_fps_arg), "--camera-fps=%d", ctx->camera_fps);
		if (ctx->camera_zoom > 0)
			snprintf(camera_zoom_arg, sizeof(camera_zoom_arg), "--camera-zoom=%.3f", ctx->camera_zoom);
		if (ctx->camera_torch)
			snprintf(camera_torch_arg, sizeof(camera_torch_arg), "--camera-torch=true");
		if (ctx->camera_iso > 0)
			snprintf(camera_iso_arg, sizeof(camera_iso_arg), "--camera-iso=%d", ctx->camera_iso);
		if (ctx->camera_shutter_us > 0)
			snprintf(camera_shutter_arg, sizeof(camera_shutter_arg), "--camera-shutter=%d", ctx->camera_shutter_us);
		if (ctx->camera_focus_distance > 0)
			snprintf(camera_focus_arg, sizeof(camera_focus_arg), "--camera-focus=%.3f", ctx->camera_focus_distance);
		if (ctx->camera_awb_mode && *ctx->camera_awb_mode && strcmp(ctx->camera_awb_mode, "auto") != 0)
			snprintf(camera_awb_arg, sizeof(camera_awb_arg), "--camera-awb=%s", ctx->camera_awb_mode);
	}

	char serial_arg[128] = {0};
	if (ctx->serial && *ctx->serial)
		snprintf(serial_arg, sizeof(serial_arg), "--serial=%s", ctx->serial);

	const char *argv[32];
	size_t n = 0;
	argv[n++] = "--no-window";
	argv[n++] = "--no-audio";
	argv[n++] = "--no-control";
	argv[n++] = bitrate_arg;
	/* Tell patched scrcpy to listen on this port and stream raw video
	 * packets (12-byte header + NAL) to the first accepted client. */
	struct dstr sink_arg = {0};
	dstr_printf(&sink_arg, "--raw-video-tcp=%u", (unsigned)port);
	argv[n++] = sink_arg.array;
	if (max_size_arg[0])
		argv[n++] = max_size_arg;
	if (codec_arg[0])
		argv[n++] = codec_arg;
	if (source_arg[0])
		argv[n++] = source_arg;
	if (camera_arg[0]) argv[n++] = camera_arg;
	if (camera_size_arg[0]) argv[n++] = camera_size_arg;
	if (camera_fps_arg[0]) argv[n++] = camera_fps_arg;
	if (camera_zoom_arg[0]) argv[n++] = camera_zoom_arg;
	if (camera_torch_arg[0]) argv[n++] = camera_torch_arg;
	if (camera_iso_arg[0]) argv[n++] = camera_iso_arg;
	if (camera_shutter_arg[0]) argv[n++] = camera_shutter_arg;
	if (camera_focus_arg[0]) argv[n++] = camera_focus_arg;
	if (camera_awb_arg[0]) argv[n++] = camera_awb_arg;
	if (serial_arg[0])
		argv[n++] = serial_arg;
	argv[n] = NULL;

	char *log_path = NULL;
	{
		const char *tmp = getenv("TEMP");
		if (!tmp || !*tmp)
			tmp = getenv("TMP");
		if (!tmp || !*tmp)
			tmp = ".";
		struct dstr lp = {0};
		dstr_printf(&lp, "%s/scrcpy-obs-child.log", tmp);
		log_path = lp.array;
	}

	obs_log(LOG_INFO, "scrcpy-source: spawning %s (port=%u, log=%s)", exe_path, (unsigned)port,
		log_path ? log_path : "(none)");

	if (scrcpy_proc_spawn(&ctx->proc, exe_path, argv, log_path)) {
		ctx->proc_alive = true;
	} else {
		obs_log(LOG_ERROR, "scrcpy-source: spawn failed");
	}
	bfree(log_path);

	ctx->reader = scrcpy_reader_create(ctx->source, port);

	dstr_free(&sink_arg);
	bfree(exe_path);
	bfree(bin_dir);
}

static void stop_scrcpy(struct scrcpy_src *ctx)
{
	if (ctx->reader) {
		scrcpy_reader_destroy(ctx->reader);
		ctx->reader = NULL;
	}
	if (ctx->proc_alive) {
		scrcpy_proc_kill(&ctx->proc);
		scrcpy_proc_close(&ctx->proc);
		ctx->proc_alive = false;
	}
}

static void load_settings(struct scrcpy_src *ctx, obs_data_t *settings)
{
	bfree(ctx->serial);
	bfree(ctx->video_source);
	bfree(ctx->codec);
	bfree(ctx->camera_size);
	bfree(ctx->camera_awb_mode);
	ctx->serial = bstrdup(obs_data_get_string(settings, "serial"));
	ctx->video_source = bstrdup(obs_data_get_string(settings, "video_source"));
	ctx->codec = bstrdup(obs_data_get_string(settings, "codec"));
	ctx->camera_id = (int)obs_data_get_int(settings, "camera_id");
	ctx->camera_size = bstrdup(obs_data_get_string(settings, "camera_size"));
	ctx->camera_fps = (int)obs_data_get_int(settings, "camera_fps");
	ctx->camera_zoom = (float)obs_data_get_double(settings, "camera_zoom");
	ctx->camera_torch = obs_data_get_bool(settings, "camera_torch");
	ctx->camera_iso = (int)obs_data_get_int(settings, "camera_iso");
	ctx->camera_shutter_us = (int)obs_data_get_int(settings, "camera_shutter_us");
	ctx->camera_focus_distance = (float)obs_data_get_double(settings, "camera_focus_distance");
	ctx->camera_awb_mode = bstrdup(obs_data_get_string(settings, "camera_awb_mode"));
	ctx->max_size = (int)obs_data_get_int(settings, "max_size");
	ctx->bitrate_kbps = (int)obs_data_get_int(settings, "bitrate_kbps");
}

static void *src_create(obs_data_t *settings, obs_source_t *source)
{
	struct scrcpy_src *ctx = bzalloc(sizeof(*ctx));
	ctx->source = source;
	load_settings(ctx, settings);

	/* Mimic OBS Video Capture Device: auto-select first available device
	 * when none is configured, so the source is immediately usable. */
	if (!ctx->serial || !*ctx->serial) {
		char *first = first_adb_serial();
		if (first) {
			bfree(ctx->serial);
			ctx->serial = first;
			obs_data_set_string(settings, "serial", first);
		}
	}

	start_scrcpy(ctx, settings);
	return ctx;
}

static void src_destroy(void *data)
{
	struct scrcpy_src *ctx = data;
	if (!ctx)
		return;
	stop_scrcpy(ctx);
	bfree(ctx->serial);
	bfree(ctx->video_source);
	bfree(ctx->codec);
	bfree(ctx->camera_size);
	bfree(ctx->camera_awb_mode);
	bfree(ctx);
}

static void src_update(void *data, obs_data_t *settings)
{
	struct scrcpy_src *ctx = data;
	stop_scrcpy(ctx);
	load_settings(ctx, settings);
	start_scrcpy(ctx, settings);
}

static void src_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "video_source", "display");
	obs_data_set_default_int(settings, "camera_id", 0);
	obs_data_set_default_string(settings, "camera_size", "1920x1080");
	obs_data_set_default_int(settings, "camera_fps", 30);
	obs_data_set_default_double(settings, "camera_zoom", 1.0);
	obs_data_set_default_bool(settings, "camera_torch", false);
	obs_data_set_default_int(settings, "camera_iso", 0);
	obs_data_set_default_int(settings, "camera_shutter_us", 0);
	obs_data_set_default_double(settings, "camera_focus_distance", 0.0);
	obs_data_set_default_string(settings, "camera_awb_mode", "auto");
	obs_data_set_default_int(settings, "max_size", 0);
	obs_data_set_default_int(settings, "bitrate_kbps", 8000);
	obs_data_set_default_string(settings, "codec", "h264");
}

static bool refresh_devices_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(data);
	obs_property_t *dev_list = obs_properties_get(props, "serial");
	if (!dev_list)
		return false;
	fill_device_list(dev_list);
	return true;
}

static bool video_source_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const char *source = obs_data_get_string(settings, "video_source");
	bool is_camera = source && strcmp(source, "camera") == 0;
	obs_property_t *camera_id = obs_properties_get(props, "camera_id");
	if (camera_id) obs_property_set_visible(camera_id, is_camera);
	const char *keys[] = {"camera_size", "camera_fps", "camera_zoom", "camera_torch", "camera_iso", "camera_shutter_us", "camera_focus_distance", "camera_awb_mode"};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
		obs_property_t *prop = obs_properties_get(props, keys[i]);
		if (prop) obs_property_set_visible(prop, is_camera);
	}
	return true;
}

static obs_properties_t *src_get_properties(void *data)
{
	struct scrcpy_src *ctx = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *dev_list = obs_properties_add_list(props, "serial", obs_module_text("Device"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_device_list(dev_list);

	obs_properties_add_button2(props, "refresh_devices", obs_module_text("RefreshDevices"), refresh_devices_clicked, NULL);

	obs_property_t *src_list = obs_properties_add_list(props, "video_source", obs_module_text("VideoSource"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(src_list, "Display", "display");
	obs_property_list_add_string(src_list, "Camera", "camera");
	obs_property_set_modified_callback(src_list, video_source_modified);

	obs_property_t *camera_id = obs_properties_add_int(props, "camera_id", obs_module_text("CameraId"), 0, 9, 1);
	obs_property_t *camera_size = obs_properties_add_list(props, "camera_size", obs_module_text("CameraSize"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(camera_size, "1920x1080", "1920x1080");
	obs_property_list_add_string(camera_size, "1920x1440", "1920x1440");
	obs_property_list_add_string(camera_size, "1280x720", "1280x720");
	obs_property_list_add_string(camera_size, "1280x960", "1280x960");
	obs_property_list_add_string(camera_size, "640x480", "640x480");
	obs_property_t *camera_fps = obs_properties_add_int(props, "camera_fps", obs_module_text("CameraFps"), 1, 60, 1);
	obs_property_t *camera_zoom = obs_properties_add_float_slider(props, "camera_zoom", obs_module_text("CameraZoom"), 1.0, 10.0, 0.1);
	obs_property_t *camera_torch = obs_properties_add_bool(props, "camera_torch", obs_module_text("CameraTorch"));
	obs_property_t *camera_iso = obs_properties_add_int_slider(props, "camera_iso", obs_module_text("CameraIso"), 0, 12800, 50);
	obs_property_t *camera_shutter = obs_properties_add_list(props, "camera_shutter_us", obs_module_text("CameraShutter"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(camera_shutter, "Auto", 0);
	obs_property_list_add_int(camera_shutter, "1/30 s", 33333);
	obs_property_list_add_int(camera_shutter, "1/60 s", 16667);
	obs_property_list_add_int(camera_shutter, "1/120 s", 8333);
	obs_property_list_add_int(camera_shutter, "1/240 s", 4167);
	obs_property_list_add_int(camera_shutter, "1/500 s", 2000);
	obs_property_list_add_int(camera_shutter, "1/1000 s", 1000);
	obs_property_list_add_int(camera_shutter, "1/2000 s", 500);
	obs_property_list_add_int(camera_shutter, "1/4000 s", 250);
	obs_property_t *camera_focus = obs_properties_add_float_slider(props, "camera_focus_distance", obs_module_text("CameraFocus"), 0.0, 20.0, 0.1);
	obs_property_t *camera_awb = obs_properties_add_list(props, "camera_awb_mode", obs_module_text("CameraWhiteBalance"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(camera_awb, "Auto", "auto");
	obs_property_list_add_string(camera_awb, "Incandescent", "incandescent");
	obs_property_list_add_string(camera_awb, "Fluorescent", "fluorescent");
	obs_property_list_add_string(camera_awb, "Daylight", "daylight");
	obs_property_list_add_string(camera_awb, "Cloudy", "cloudy");
	obs_property_set_visible(camera_id, ctx->video_source && strcmp(ctx->video_source, "camera") == 0);
	obs_property_set_visible(camera_size, ctx->video_source && strcmp(ctx->video_source, "camera") == 0);
	obs_property_set_visible(camera_fps, ctx->video_source && strcmp(ctx->video_source, "camera") == 0);
	obs_property_set_visible(camera_zoom, ctx->video_source && strcmp(ctx->video_source, "camera") == 0);
	obs_property_set_visible(camera_torch, ctx->video_source && strcmp(ctx->video_source, "camera") == 0);
	obs_property_set_visible(camera_iso, ctx->video_source && strcmp(ctx->video_source, "camera") == 0);
	obs_property_set_visible(camera_shutter, ctx->video_source && strcmp(ctx->video_source, "camera") == 0);
	obs_property_set_visible(camera_focus, ctx->video_source && strcmp(ctx->video_source, "camera") == 0);
	obs_property_set_visible(camera_awb, ctx->video_source && strcmp(ctx->video_source, "camera") == 0);

	obs_properties_add_int(props, "max_size", obs_module_text("MaxSize"), 0, 4096, 16);

	obs_properties_add_int(props, "bitrate_kbps", obs_module_text("BitrateKbps"), 500, 50000, 500);

	obs_property_t *codec_list = obs_properties_add_list(props, "codec", obs_module_text("Codec"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(codec_list, "H.264", "h264");
	obs_property_list_add_string(codec_list, "H.265", "h265");
	obs_property_list_add_string(codec_list, "AV1", "av1");

	return props;
}

struct obs_source_info scrcpy_source_info = {
	.id = "scrcpy_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.icon_type = OBS_ICON_TYPE_CAMERA,
	.get_name = src_get_name,
	.create = src_create,
	.destroy = src_destroy,
	.update = src_update,
	.get_defaults = src_get_defaults,
	.get_properties = src_get_properties,
};
