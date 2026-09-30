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
#include "scrcpy-camera-control.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <util/platform.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define SCRCPY_EXE_NAME "scrcpy.exe"

#define CAMERA_COLOR_PROFILE_AUTO 0
#define CAMERA_COLOR_SPACE_SRGB 1
#define CAMERA_COLOR_SPACE_REC709 2
#define CAMERA_COLOR_SPACE_REC2020 3

#define CAMERA_GAMMA_22 1
#define CAMERA_GAMMA_24 2
#define CAMERA_GAMMA_REC709_SCENE 3
#define CAMERA_GAMMA_REC709_A 4
#define CAMERA_GAMMA_HLG 5
#define CAMERA_GAMMA_HDR10 6
#define CAMERA_GAMMA_HDR10_PLUS 7
#define CAMERA_GAMMA_SRGB 8
#define CAMERA_GAMMA_LINEAR 9

#define CAMERA_COLOR_PROFILE_SRGB_22 11
#define CAMERA_COLOR_PROFILE_SRGB_24 12
#define CAMERA_COLOR_PROFILE_SRGB_REC709_SCENE 13
#define CAMERA_COLOR_PROFILE_SRGB_REC709_A 14
#define CAMERA_COLOR_PROFILE_REC709_22 21
#define CAMERA_COLOR_PROFILE_REC709_24 22
#define CAMERA_COLOR_PROFILE_REC709_REC709_SCENE 23
#define CAMERA_COLOR_PROFILE_REC709_REC709_A 24
#define CAMERA_COLOR_PROFILE_REC2020_22 31
#define CAMERA_COLOR_PROFILE_REC2020_24 32
#define CAMERA_COLOR_PROFILE_REC2020_REC709_SCENE 33
#define CAMERA_COLOR_PROFILE_REC2020_REC709_A 34
#define CAMERA_COLOR_PROFILE_REC2020_HLG 35
#define CAMERA_COLOR_PROFILE_REC2020_HDR10 36
#define CAMERA_COLOR_PROFILE_REC2020_HDR10_PLUS 37
#define CAMERA_COLOR_PROFILE_SRGB_SRGB 18
#define CAMERA_COLOR_PROFILE_SRGB_LINEAR 19
#define CAMERA_COLOR_PROFILE_REC709_SRGB 28
#define CAMERA_COLOR_PROFILE_REC709_LINEAR 29
#define CAMERA_COLOR_PROFILE_REC2020_SRGB 38
#define CAMERA_COLOR_PROFILE_REC2020_LINEAR 39

#define CAMERA_DYNAMIC_RANGE_STANDARD 0
#define CAMERA_DYNAMIC_RANGE_HLG10 1
#define CAMERA_DYNAMIC_RANGE_HDR10 2
#define CAMERA_DYNAMIC_RANGE_HDR10_PLUS 3

static int camera_gamma_to_dynamic_range(int gamma, bool ten_bit)
{
	if (!ten_bit)
		return CAMERA_DYNAMIC_RANGE_STANDARD;
	if (gamma == CAMERA_GAMMA_HDR10)
		return CAMERA_DYNAMIC_RANGE_HDR10;
	if (gamma == CAMERA_GAMMA_HDR10_PLUS)
		return CAMERA_DYNAMIC_RANGE_HDR10_PLUS;
	return CAMERA_DYNAMIC_RANGE_HLG10;
}

static bool camera_color_profile_to_components(int profile, int *color_space, int *gamma)
{
	if (!color_space || !gamma)
		return false;

	*color_space = 0;
	*gamma = 0;

	if (profile <= 0)
		return true;

	*color_space = profile / 10;
	*gamma = profile % 10;
	if (*color_space < CAMERA_COLOR_SPACE_SRGB || *color_space > CAMERA_COLOR_SPACE_REC2020 ||
	    *gamma < CAMERA_GAMMA_22 || *gamma > CAMERA_GAMMA_LINEAR) {
		*color_space = 0;
		*gamma = 0;
		return false;
	}
	return true;
}

static void refresh_camera_capabilities_cache(const char *serial, bool force);

struct scrcpy_src {
	obs_source_t *source;
	scrcpy_proc_t proc;
	bool proc_alive;
	scrcpy_reader_t *reader;
	uint16_t port;
	uint16_t camera_control_port;
	scrcpy_camera_control_t *camera_control;

	char *serial;
	char *video_source;
	char *camera_id;
	char *camera_size;
	int camera_fps;
	float camera_zoom;
	bool camera_torch;
	int camera_iso;
	int camera_shutter_us;
	float camera_focus_distance;
	int camera_wb_kelvin;
	bool camera_wb_lock;
	int camera_color_profile;
	int camera_color_range;
	bool camera_10bit;
	bool portrait_mode;

	pthread_mutex_t state_mutex;
	pthread_t watchdog_thread;
	bool watchdog_started;
	volatile bool watchdog_stop;
	volatile bool updating;

	int max_size;
	int bitrate_kbps;
	char *codec;

	bool hardware_decoding;
	bool flip_vertical;
	int video_buffer_ms;
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
	UNUSED_PARAMETER(settings);

	uint16_t port = 0;
	if (!pick_ephemeral_port(&port)) {
		obs_log(LOG_ERROR, "scrcpy-source: could not pick video port");
		return;
	}
	ctx->port = port;

	uint16_t control_port = 0;
	if (ctx->video_source && strcmp(ctx->video_source, "camera") == 0) {
		if (!pick_ephemeral_port(&control_port)) {
			obs_log(LOG_ERROR, "scrcpy-source: could not pick camera control port");
			return;
		}
		ctx->camera_control_port = control_port;
	} else {
		ctx->camera_control_port = 0;
	}

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
	const char *effective_codec = ctx->camera_10bit ? "h265" : ctx->codec;
	if (effective_codec && *effective_codec)
		snprintf(codec_arg, sizeof(codec_arg), "--video-codec=%s", effective_codec);
	char source_arg[64] = {0};
	if (ctx->video_source && *ctx->video_source)
		snprintf(source_arg, sizeof(source_arg), "--video-source=%s", ctx->video_source);

	char camera_arg[64] = {0};
	char camera_size_arg[64] = {0};
	char camera_fps_arg[64] = {0};
	char control_codec_arg[1024] = {0};
	if (ctx->video_source && strcmp(ctx->video_source, "camera") == 0) {
		snprintf(camera_arg, sizeof(camera_arg), "--camera-id=%s", ctx->camera_id ? ctx->camera_id : "0");
		if (ctx->camera_size && *ctx->camera_size)
			snprintf(camera_size_arg, sizeof(camera_size_arg), "--camera-size=%s", ctx->camera_size);
		if (ctx->camera_fps > 0)
			snprintf(camera_fps_arg, sizeof(camera_fps_arg), "--camera-fps=%d", ctx->camera_fps);
		/* Keep startup Camera2 and encoder configuration independent from
		 * runtime camera controls. */
		int startup_color_space = 0;
		int startup_gamma = 0;
		if (!camera_color_profile_to_components(ctx->camera_color_profile, &startup_color_space,
							&startup_gamma)) {
			startup_color_space = 0;
			startup_gamma = 0;
		}
		if (ctx->camera_10bit &&
		    (startup_gamma < CAMERA_GAMMA_HLG || startup_gamma > CAMERA_GAMMA_HDR10_PLUS)) {
			startup_color_space = CAMERA_COLOR_SPACE_REC2020;
			startup_gamma = CAMERA_GAMMA_HLG;
		}
		snprintf(
			control_codec_arg, sizeof(control_codec_arg),
			"--video-codec-options=__scrcpy_obs_camera_control_port:int=%u,__scrcpy_obs_camera_iso:int=%d,__scrcpy_obs_camera_shutter_us:int=%d,__scrcpy_obs_camera_focus_distance:float=%.6f,__scrcpy_obs_camera_wb_kelvin:int=%d,__scrcpy_obs_camera_wb_lock:int=%d,__scrcpy_obs_camera_color_space:int=%d,__scrcpy_obs_camera_gamma:int=%d%s",
			(unsigned)control_port, ctx->camera_iso, ctx->camera_shutter_us, ctx->camera_focus_distance,
			ctx->camera_wb_kelvin, ctx->camera_wb_lock ? 1 : 0, startup_color_space, startup_gamma,
			ctx->camera_10bit ? ",__scrcpy_obs_camera_10bit:int=1" : "");
	}

	char serial_arg[128] = {0};
	if (ctx->serial && *ctx->serial)
		snprintf(serial_arg, sizeof(serial_arg), "--serial=%s", ctx->serial);

	const char *argv[24];
	size_t n = 0;
	argv[n++] = "--no-window";
	argv[n++] = "--no-audio";
	argv[n++] = "--no-control";
	argv[n++] = bitrate_arg;

	struct dstr sink_arg = {0};
	dstr_printf(&sink_arg, "--raw-video-tcp=%u", (unsigned)port);
	argv[n++] = sink_arg.array;
	if (max_size_arg[0])
		argv[n++] = max_size_arg;
	if (codec_arg[0])
		argv[n++] = codec_arg;
	if (source_arg[0])
		argv[n++] = source_arg;
	if (camera_arg[0])
		argv[n++] = camera_arg;
	if (camera_size_arg[0])
		argv[n++] = camera_size_arg;
	if (camera_fps_arg[0])
		argv[n++] = camera_fps_arg;
	if (control_codec_arg[0])
		argv[n++] = control_codec_arg;
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

	obs_log(LOG_INFO, "scrcpy-source: spawning %s (video-port=%u, control-port=%u, log=%s)", exe_path,
		(unsigned)port, (unsigned)control_port, log_path ? log_path : "(none)");

	if (scrcpy_proc_spawn(&ctx->proc, exe_path, argv, log_path)) {
		ctx->proc_alive = true;
	} else {
		obs_log(LOG_ERROR, "scrcpy-source: spawn failed");
	}
	bfree(log_path);

	ctx->reader = scrcpy_reader_create(ctx->source, port, ctx->hardware_decoding, ctx->flip_vertical,
					   ctx->video_buffer_ms, ctx->portrait_mode, ctx->camera_color_range);

	if (ctx->video_source && strcmp(ctx->video_source, "camera") == 0 && control_port != 0 && ctx->serial &&
	    *ctx->serial) {
		ctx->camera_control = scrcpy_camera_control_create(ctx->serial, control_port);
		if (ctx->camera_control) {
			int startup_color_space = 0;
			int startup_gamma = 0;
			if (!camera_color_profile_to_components(ctx->camera_color_profile, &startup_color_space,
								&startup_gamma)) {
				startup_color_space = 0;
				startup_gamma = 0;
			}
			if (ctx->camera_10bit &&
			    (startup_gamma < CAMERA_GAMMA_HLG || startup_gamma > CAMERA_GAMMA_HDR10_PLUS)) {
				startup_color_space = CAMERA_COLOR_SPACE_REC2020;
				startup_gamma = CAMERA_GAMMA_HLG;
			}
			if (!scrcpy_camera_control_apply(
				    ctx->camera_control, ctx->camera_zoom, ctx->camera_torch, ctx->camera_iso,
				    ctx->camera_shutter_us, ctx->camera_focus_distance, ctx->camera_wb_kelvin,
				    ctx->camera_wb_lock, startup_color_space, startup_gamma, ctx->camera_10bit,
				    camera_gamma_to_dynamic_range(startup_gamma, ctx->camera_10bit))) {
				obs_log(LOG_WARNING, "scrcpy-source: camera control connection not ready");
			}
		}
	}

	dstr_free(&sink_arg);
	bfree(exe_path);
	bfree(bin_dir);
}

static void stop_scrcpy(struct scrcpy_src *ctx)
{
	if (ctx->camera_control) {
		scrcpy_camera_control_destroy(ctx->camera_control);
		ctx->camera_control = NULL;
	}
	ctx->camera_control_port = 0;
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
	bfree(ctx->camera_id);
	bfree(ctx->codec);
	bfree(ctx->camera_size);
	ctx->serial = bstrdup(obs_data_get_string(settings, "serial"));
	ctx->video_source = bstrdup(obs_data_get_string(settings, "video_source"));
	ctx->codec = bstrdup(obs_data_get_string(settings, "codec"));
	const char *camera_id = obs_data_get_string(settings, "camera_id");
	if (camera_id && *camera_id) {
		ctx->camera_id = bstrdup(camera_id);
	} else {
		char camera_id_buf[32];
		snprintf(camera_id_buf, sizeof(camera_id_buf), "%lld",
			 (long long)obs_data_get_int(settings, "camera_id"));
		ctx->camera_id = bstrdup(camera_id_buf);
		obs_data_set_string(settings, "camera_id", camera_id_buf);
	}
	ctx->camera_size = bstrdup(obs_data_get_string(settings, "camera_size"));
	ctx->camera_fps = (int)obs_data_get_int(settings, "camera_fps");
	ctx->camera_zoom = (float)obs_data_get_double(settings, "camera_zoom");
	ctx->camera_torch = obs_data_get_bool(settings, "camera_torch");
	ctx->camera_iso = (int)obs_data_get_int(settings, "camera_iso");
	ctx->camera_shutter_us = (int)obs_data_get_int(settings, "camera_shutter_us");
	ctx->camera_focus_distance = (float)obs_data_get_double(settings, "camera_focus_distance");
	ctx->camera_wb_kelvin = (int)obs_data_get_int(settings, "camera_wb_kelvin");
	ctx->camera_wb_lock = obs_data_get_bool(settings, "camera_wb_lock");
	ctx->camera_color_profile = (int)obs_data_get_int(settings, "camera_color_profile");
	ctx->camera_color_range = (int)obs_data_get_int(settings, "camera_color_range");
	ctx->camera_10bit = obs_data_get_bool(settings, "camera_10bit");
	ctx->portrait_mode = obs_data_get_bool(settings, "portrait_mode");
	ctx->max_size = (int)obs_data_get_int(settings, "max_size");
	ctx->bitrate_kbps = (int)obs_data_get_int(settings, "bitrate_kbps");
	ctx->hardware_decoding = obs_data_get_bool(settings, "hardware_decoding");
	ctx->flip_vertical = obs_data_get_bool(settings, "flip_vertical");
	ctx->video_buffer_ms = (int)obs_data_get_int(settings, "video_buffer_ms");
}

static bool scrcpy_stream_healthy(struct scrcpy_src *ctx)
{
	if (!ctx->proc_alive || !scrcpy_proc_alive(&ctx->proc))
		return false;
	if (!scrcpy_reader_is_alive(ctx->reader))
		return false;

	/* A USB/ADB disconnect may leave the child alive while its socket
	 * remains open. Treat a stream with no decoded frame for a few
	 * seconds as dead so the watchdog can restart it. */
	return !scrcpy_reader_is_stale(ctx->reader, 3000);
}

static void *scrcpy_watchdog(void *data)
{
	struct scrcpy_src *ctx = data;
	os_set_thread_name("scrcpy-watchdog");

	while (!os_atomic_load_bool(&ctx->watchdog_stop)) {
		os_sleep_ms(500);
		if (os_atomic_load_bool(&ctx->watchdog_stop) || os_atomic_load_bool(&ctx->updating))
			continue;

		if (scrcpy_stream_healthy(ctx))
			continue;

		pthread_mutex_lock(&ctx->state_mutex);
		if (!os_atomic_load_bool(&ctx->watchdog_stop) && !os_atomic_load_bool(&ctx->updating) &&
		    !scrcpy_stream_healthy(ctx)) {
			obs_log(LOG_WARNING, "scrcpy-source: video stream lost; restarting scrcpy");
			obs_source_output_video(ctx->source, NULL);

			stop_scrcpy(ctx);
			obs_data_t *settings = obs_source_get_settings(ctx->source);
			load_settings(ctx, settings);
			start_scrcpy(ctx, settings);
			if (ctx->video_source && strcmp(ctx->video_source, "camera") == 0 && ctx->serial &&
			    *ctx->serial)
				refresh_camera_capabilities_cache(ctx->serial, true);
			obs_data_release(settings);
		}
		pthread_mutex_unlock(&ctx->state_mutex);
	}

	return NULL;
}

static void *src_create(obs_data_t *settings, obs_source_t *source)
{
	struct scrcpy_src *ctx = bzalloc(sizeof(*ctx));
	ctx->source = source;
	pthread_mutex_init(&ctx->state_mutex, NULL);
	ctx->watchdog_stop = false;
	ctx->updating = false;
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

	/* Prime capabilities after auto-selecting the device, but before OBS can
	 * open the properties dialog. */
	if (ctx->video_source && strcmp(ctx->video_source, "camera") == 0 && ctx->serial && *ctx->serial)
		refresh_camera_capabilities_cache(ctx->serial, false);

	start_scrcpy(ctx, settings);
	if (pthread_create(&ctx->watchdog_thread, NULL, scrcpy_watchdog, ctx) == 0)
		ctx->watchdog_started = true;
	else
		obs_log(LOG_WARNING, "scrcpy-source: could not start watchdog thread");
	return ctx;
}

static void src_destroy(void *data)
{
	struct scrcpy_src *ctx = data;
	if (!ctx)
		return;
	os_atomic_set_bool(&ctx->watchdog_stop, true);
	if (ctx->watchdog_started) {
		pthread_join(ctx->watchdog_thread, NULL);
		ctx->watchdog_started = false;
	}
	stop_scrcpy(ctx);
	bfree(ctx->serial);
	bfree(ctx->video_source);
	bfree(ctx->codec);
	bfree(ctx->camera_size);
	pthread_mutex_destroy(&ctx->state_mutex);
	bfree(ctx);
}

static bool setting_string_changed(const char *old_value, const char *new_value)
{
	if (!old_value)
		old_value = "";
	if (!new_value)
		new_value = "";
	return strcmp(old_value, new_value) != 0;
}

static bool camera_restart_required(const struct scrcpy_src *ctx, obs_data_t *settings)
{
	return setting_string_changed(ctx->serial, obs_data_get_string(settings, "serial")) ||
	       setting_string_changed(ctx->video_source, obs_data_get_string(settings, "video_source")) ||
	       setting_string_changed(ctx->camera_id, obs_data_get_string(settings, "camera_id")) ||
	       setting_string_changed(ctx->camera_size, obs_data_get_string(settings, "camera_size")) ||
	       ctx->camera_fps != (int)obs_data_get_int(settings, "camera_fps") ||
	       ctx->max_size != (int)obs_data_get_int(settings, "max_size") ||
	       ctx->bitrate_kbps != (int)obs_data_get_int(settings, "bitrate_kbps") ||
	       setting_string_changed(ctx->codec, obs_data_get_string(settings, "codec")) ||
	       ctx->hardware_decoding != obs_data_get_bool(settings, "hardware_decoding") ||
	       ctx->flip_vertical != obs_data_get_bool(settings, "flip_vertical") ||
	       ctx->portrait_mode != obs_data_get_bool(settings, "portrait_mode") ||
	       ctx->video_buffer_ms != (int)obs_data_get_int(settings, "video_buffer_ms") ||
	       ctx->camera_color_profile != (int)obs_data_get_int(settings, "camera_color_profile") ||
	       ctx->camera_10bit != obs_data_get_bool(settings, "camera_10bit");
}

static void src_update(void *data, obs_data_t *settings)
{
	struct scrcpy_src *ctx = data;

	pthread_mutex_lock(&ctx->state_mutex);
	os_atomic_set_bool(&ctx->updating, true);

	if (camera_restart_required(ctx, settings)) {
		stop_scrcpy(ctx);
		load_settings(ctx, settings);
		start_scrcpy(ctx, settings);
		if (ctx->video_source && strcmp(ctx->video_source, "camera") == 0 && ctx->serial && *ctx->serial)
			refresh_camera_capabilities_cache(ctx->serial, false);
	} else {
		load_settings(ctx, settings);
		if (ctx->video_source && strcmp(ctx->video_source, "camera") == 0) {
			if (!ctx->camera_control && ctx->serial && *ctx->serial && ctx->camera_control_port != 0)
				ctx->camera_control =
					scrcpy_camera_control_create(ctx->serial, ctx->camera_control_port);
		}
	}

	if (ctx->reader)
		scrcpy_reader_set_color_range(ctx->reader, ctx->camera_color_range);

	if (ctx->video_source && strcmp(ctx->video_source, "camera") == 0 && ctx->camera_control) {
		int runtime_color_space = 0;
		int runtime_gamma = 0;
		if (!camera_color_profile_to_components(ctx->camera_color_profile, &runtime_color_space,
							&runtime_gamma)) {
			runtime_color_space = 0;
			runtime_gamma = 0;
		}
		if (ctx->camera_10bit &&
		    (runtime_gamma < CAMERA_GAMMA_HLG || runtime_gamma > CAMERA_GAMMA_HDR10_PLUS)) {
			runtime_color_space = CAMERA_COLOR_SPACE_REC2020;
			runtime_gamma = CAMERA_GAMMA_HLG;
		}
		(void)scrcpy_camera_control_apply(ctx->camera_control, ctx->camera_zoom, ctx->camera_torch, ctx->camera_iso,
						  ctx->camera_shutter_us, ctx->camera_focus_distance, ctx->camera_wb_kelvin,
						  ctx->camera_wb_lock, runtime_color_space, runtime_gamma, ctx->camera_10bit,
						  camera_gamma_to_dynamic_range(runtime_gamma, ctx->camera_10bit));
	}

	os_atomic_set_bool(&ctx->updating, false);
	pthread_mutex_unlock(&ctx->state_mutex);
}

static void src_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "video_source", "display");
	obs_data_set_default_string(settings, "camera_id", "0");
	obs_data_set_default_string(settings, "camera_size", "1920x1080");
	obs_data_set_default_int(settings, "camera_fps", 30);
	obs_data_set_default_double(settings, "camera_zoom", 0.6);
	obs_data_set_default_bool(settings, "camera_torch", false);
	obs_data_set_default_int(settings, "camera_iso", 0);
	obs_data_set_default_int(settings, "camera_shutter_us", 0);
	obs_data_set_default_double(settings, "camera_focus_distance", 0.0);
	obs_data_set_default_int(settings, "camera_wb_kelvin", 0);
	obs_data_set_default_bool(settings, "camera_wb_lock", false);
	obs_data_set_default_int(settings, "camera_color_profile", CAMERA_COLOR_PROFILE_AUTO);
	obs_data_set_default_int(settings, "camera_color_range", SCRCPY_COLOR_RANGE_AUTO);
	obs_data_set_default_bool(settings, "camera_10bit", false);
	obs_data_set_default_bool(settings, "portrait_mode", false);
	obs_data_set_default_int(settings, "max_size", 0);
	obs_data_set_default_int(settings, "bitrate_kbps", 8000);
	obs_data_set_default_string(settings, "codec", "h264");
	obs_data_set_default_bool(settings, "hardware_decoding", true);
	obs_data_set_default_bool(settings, "flip_vertical", false);
	obs_data_set_default_int(settings, "video_buffer_ms", 0);
}

static void populate_shutter_list(obs_property_t *prop, long long min_exposure_ns, long long max_exposure_ns);

static bool run_scrcpy_camera_query(const char *serial, char **output)
{
	if (!serial || !*serial)
		return false;

	char *bin_dir = get_bin_dir();
	if (!bin_dir)
		return false;

	char *exe_path = path_join(bin_dir, SCRCPY_EXE_NAME);
	char serial_arg[256];
	snprintf(serial_arg, sizeof(serial_arg), "--serial=%s", serial);

	const char *argv[] = {
		"--list-cameras",
		"--list-camera-sizes",
		serial_arg,
		NULL,
	};
	bool ok = scrcpy_proc_run_capture(exe_path, argv, output);

	bfree(exe_path);
	bfree(bin_dir);
	return ok;
}

static char *g_camera_capabilities_serial;
static char *g_camera_capabilities_output;
static pthread_mutex_t g_camera_capabilities_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool parse_camera_id_line(const char *line, char *id, size_t id_size, char *label, size_t label_size, int *fps,
				 size_t *fps_count, float *focus_max, float *zoom_min, float *zoom_max, int *wb_min,
				 int *wb_max, bool *wb_manual)
{
	const char *p = strstr(line, "--camera-id=");
	if (!p)
		return false;

	p += strlen("--camera-id=");
	const char *end = p;
	while (*end && !isspace((unsigned char)*end))
		end++;

	size_t len = (size_t)(end - p);
	if (len == 0 || len >= id_size)
		return false;

	memcpy(id, p, len);
	id[len] = '\0';

	char facing[32] = "unknown";
	unsigned width = 0;
	unsigned height = 0;
	const char *open = strchr(end, '(');
	if (open) {
		const char *comma = strchr(open + 1, ',');
		if (comma) {
			size_t flen = (size_t)(comma - (open + 1));
			while (flen && isspace((unsigned char)open[1 + flen - 1]))
				flen--;
			if (flen >= sizeof(facing))
				flen = sizeof(facing) - 1;
			memcpy(facing, open + 1, flen);
			facing[flen] = '\0';

			const char *size_start = comma + 1;
			while (*size_start && isspace((unsigned char)*size_start))
				size_start++;
			sscanf(size_start, "%ux%u", &width, &height);
		}
	}

	if (label && label_size > 0)
		if (width > 0 && height > 0)
			snprintf(label, label_size, "Camera %s (%s, %ux%u)", id, facing, width, height);
		else
			snprintf(label, label_size, "Camera %s (%s)", id, facing);

	if (fps && fps_count) {
		*fps_count = 0;
		const char *fps_start = strstr(line, "fps=");
		if (fps_start) {
			fps_start += 4;
			while (*fps_start && *fps_start != '}' && *fps_start != ']') {
				if (isdigit((unsigned char)*fps_start)) {
					char *next;
					long value = strtol(fps_start, &next, 10);
					if (value > 0 && value <= 1000 && *fps_count < 32) {
						bool duplicate = false;
						for (size_t i = 0; i < *fps_count; ++i) {
							if (fps[i] == (int)value) {
								duplicate = true;
								break;
							}
						}
						if (!duplicate)
							fps[(*fps_count)++] = (int)value;
					}
					fps_start = next;
				} else {
					fps_start++;
				}
			}
		}
	}

	if (focus_max) {
		*focus_max = 0.0f;
		const char *focus_start = strstr(line, "focus-range=[");
		if (focus_start) {
			focus_start += strlen("focus-range=[");
			float lower = 0.0f;
			float upper = 0.0f;
			if (sscanf(focus_start, "%f, %f", &lower, &upper) == 2 && upper > 0.0f)
				*focus_max = upper;
		}
	}

	if (zoom_min && zoom_max) {
		*zoom_min = 1.0f;
		*zoom_max = 1.0f;
		const char *zoom_start = strstr(line, "zoom-range=[");
		if (zoom_start) {
			zoom_start += strlen("zoom-range=[");
			if (sscanf(zoom_start, "%f, %f", zoom_min, zoom_max) != 2 || *zoom_min <= 0.0f ||
			    *zoom_max < *zoom_min) {
				*zoom_min = 1.0f;
				*zoom_max = 1.0f;
			}
		}
	}

	if (wb_min && wb_max) {
		*wb_min = 0;
		*wb_max = 0;
		const char *wb_start = strstr(line, "wb-kelvin-range=[");
		if (wb_start) {
			wb_start += strlen("wb-kelvin-range=[");
			if (sscanf(wb_start, "%d, %d", wb_min, wb_max) != 2 || *wb_min <= 0 || *wb_max <= *wb_min) {
				*wb_min = 0;
				*wb_max = 0;
			}
		}
	}

	if (wb_manual)
		*wb_manual = strstr(line, "wb-kelvin-range=[") != NULL || strstr(line, "wb-presets=true") != NULL ||
			     strstr(line, "wb-manual=true") != NULL;

	return true;
}

static bool color_space_list_contains(const char *list, const char *token)
{
	if (!list || !token)
		return false;

	const char *p = list;
	while (*p) {
		while (*p == ' ' || *p == '\t')
			p++;

		const char *end = strchr(p, ',');
		if (!end)
			end = p + strlen(p);

		const char *trim_end = end;
		while (trim_end > p && (trim_end[-1] == ' ' || trim_end[-1] == '\t'))
			trim_end--;

		size_t len = (size_t)(trim_end - p);
		if (strlen(token) == len && strncmp(p, token, len) == 0)
			return true;

		if (!*end)
			break;
		p = end + 1;
	}
	return false;
}

static bool dynamic_range_list_contains(const char *line, const char *token)
{
	if (!line || !token)
		return false;

	const char *start = strstr(line, "dynamic-range-profiles=[");
	if (!start)
		return false;
	start += strlen("dynamic-range-profiles=[");
	const char *end = strchr(start, ']');
	if (!end)
		return false;

	size_t len = (size_t)(end - start);
	char list[256];
	if (len >= sizeof(list))
		len = sizeof(list) - 1;
	memcpy(list, start, len);
	list[len] = '\0';
	return color_space_list_contains(list, token);
}

static bool parse_camera_color_capabilities(const char *line, bool *srgb, bool *rec709, bool *rec2020,
					    bool *tone_map_gamma, bool *tone_map_rec709, bool *tone_map_srgb,
					    bool *tone_map_contrast, bool *tenbit_hlg, bool *tenbit_hdr10,
					    bool *tenbit_hdr10_plus)
{
	if (!line)
		return false;

	if (srgb)
		*srgb = false;
	if (rec709)
		*rec709 = false;
	if (rec2020)
		*rec2020 = false;
	if (tone_map_gamma)
		*tone_map_gamma = false;
	if (tone_map_rec709)
		*tone_map_rec709 = false;
	if (tone_map_srgb)
		*tone_map_srgb = false;
	if (tone_map_contrast)
		*tone_map_contrast = false;
	if (tenbit_hlg)
		*tenbit_hlg = false;
	if (tenbit_hdr10)
		*tenbit_hdr10 = false;
	if (tenbit_hdr10_plus)
		*tenbit_hdr10_plus = false;

	const char *spaces = strstr(line, "standard-color-spaces=[");
	if (spaces) {
		spaces += strlen("standard-color-spaces=[");
		const char *end = strchr(spaces, ']');
		if (!end)
			end = spaces + strlen(spaces);

		char list[512];
		size_t len = (size_t)(end - spaces);
		if (len >= sizeof(list))
			len = sizeof(list) - 1;
		memcpy(list, spaces, len);
		list[len] = '\0';

		if (srgb)
			*srgb = color_space_list_contains(list, "SRGB");
		if (rec709)
			*rec709 = color_space_list_contains(list, "BT709");
		if (rec2020)
			*rec2020 = color_space_list_contains(list, "BT2020");
	}

	if (tone_map_gamma)
		*tone_map_gamma = strstr(line, "tonemap-gamma=true") != NULL;
	if (tone_map_rec709)
		*tone_map_rec709 = strstr(line, "tonemap-rec709=true") != NULL;
	if (tone_map_srgb)
		*tone_map_srgb = strstr(line, "tonemap-srgb=true") != NULL;
	if (tone_map_contrast)
		*tone_map_contrast = strstr(line, "tonemap-contrast=true") != NULL;
	if (tenbit_hlg)
		*tenbit_hlg = dynamic_range_list_contains(line, "HLG10");
	if (tenbit_hdr10)
		*tenbit_hdr10 = dynamic_range_list_contains(line, "HDR10");
	if (tenbit_hdr10_plus)
		*tenbit_hdr10_plus = dynamic_range_list_contains(line, "HDR10_PLUS");

	return spaces != NULL;
}

static void parse_camera_sensor_ranges(const char *line, int *iso_min, int *iso_max, long long *exposure_min_ns,
				       long long *exposure_max_ns)
{
	if (!line)
		return;

	const char *iso_start = strstr(line, "iso-range=[");
	if (iso_start && iso_min && iso_max) {
		iso_start += strlen("iso-range=[");
		int lo = 0;
		int hi = 0;
		if (sscanf(iso_start, "%d, %d", &lo, &hi) == 2 && lo > 0 && hi >= lo) {
			*iso_min = lo;
			*iso_max = hi;
		}
	}

	const char *exp_start = strstr(line, "exposure-time-range-ns=[");
	if (exp_start && exposure_min_ns && exposure_max_ns) {
		exp_start += strlen("exposure-time-range-ns=[");
		long long lo = 0;
		long long hi = 0;
		if (sscanf(exp_start, "%lld, %lld", &lo, &hi) == 2 && lo > 0 && hi >= lo) {
			*exposure_min_ns = lo;
			*exposure_max_ns = hi;
		}
	}
}

static void parse_camera_post_raw_boost_range(const char *line, int *boost_min, int *boost_max)
{
	if (!line || !boost_min || !boost_max)
		return;

	const char *start = strstr(line, "post-raw-sensitivity-boost-range=[");
	if (!start)
		return;

	start += strlen("post-raw-sensitivity-boost-range=[");
	int lo = 100;
	int hi = 100;
	if (sscanf(start, "%d, %d", &lo, &hi) == 2 && lo > 0 && hi >= lo) {
		*boost_min = lo;
		*boost_max = hi;
	}
}

static bool parse_selected_camera_sizes(const char *output, const char *selected_id, obs_property_t *resolution)
{
	bool in_camera = false;
	bool high_speed = false;
	size_t count = 0;

	const char *line = output;
	while (line && *line) {
		const char *end = strpbrk(line, "\r\n");
		size_t line_len = end ? (size_t)(end - line) : strlen(line);

		char line_copy[4096];
		if (line_len >= sizeof(line_copy))
			line_len = sizeof(line_copy) - 1;
		memcpy(line_copy, line, line_len);
		line_copy[line_len] = '\0';

		char camera_id[64];
		if (parse_camera_id_line(line_copy, camera_id, sizeof(camera_id), NULL, 0, NULL, NULL, NULL, NULL, NULL,
					 NULL, NULL, NULL)) {
			in_camera = strcmp(camera_id, selected_id) == 0;
			high_speed = false;
		} else if (in_camera) {
			if (strstr(line_copy, "High speed capture") != NULL) {
				high_speed = true;
			} else if (!high_speed) {
				const char *dash = strstr(line_copy, "- ");
				if (dash) {
					unsigned width = 0;
					unsigned height = 0;
					if (sscanf(dash + 2, "%ux%u", &width, &height) == 2 && width > 0 &&
					    height > 0) {
						char size[32];
						snprintf(size, sizeof(size), "%ux%u", width, height);

						bool duplicate = false;
						for (size_t i = 0; i < count; ++i) {
							const char *existing =
								obs_property_list_item_string(resolution, i);
							if (existing && strcmp(existing, size) == 0) {
								duplicate = true;
								break;
							}
						}
						if (!duplicate && count < 256) {
							obs_property_list_add_string(resolution, size, size);
							count++;
						}
					}
				}
			}
		}

		if (!end)
			break;
		line = end;
		while (*line == '\r' || *line == '\n')
			line++;
	}

	return count > 0;
}

static void populate_camera_fallbacks(obs_property_t *camera_id_prop, obs_property_t *resolution_prop,
				      obs_property_t *fps_prop, obs_property_t *color_profile_prop)
{
	if (obs_property_list_item_count(camera_id_prop) == 0) {
		for (int id = 0; id < 5; ++id) {
			char label[32];
			char value[16];
			snprintf(value, sizeof(value), "%d", id);
			snprintf(label, sizeof(label), "Camera %d", id);
			obs_property_list_add_string(camera_id_prop, label, value);
		}
	}

	if (obs_property_list_item_count(resolution_prop) == 0) {
		static const char *const sizes[] = {
			"4096x3072", "3840x2160", "3264x2448", "2560x1440", "1920x1440",
			"1920x1080", "1280x960",  "1280x720",  "640x480",
		};
		for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
			obs_property_list_add_string(resolution_prop, sizes[i], sizes[i]);
	}

	if (obs_property_list_item_count(fps_prop) == 0) {
		obs_property_list_add_int(fps_prop, "30 fps", 30);
	}
	if (color_profile_prop && obs_property_list_item_count(color_profile_prop) == 0)
		obs_property_list_add_int(color_profile_prop, "Camera-provided / Auto (default)",
					  CAMERA_COLOR_PROFILE_AUTO);
}

static void add_unique_fps(int *values, size_t *count, int value)
{
	if (!values || !count || value < 1 || value > 240 || *count >= 64)
		return;
	for (size_t i = 0; i < *count; ++i) {
		if (values[i] == value)
			return;
	}
	values[(*count)++] = value;
}

static void refresh_camera_capabilities_cache(const char *serial, bool force)
{
	if (!serial || !*serial)
		return;

	pthread_mutex_lock(&g_camera_capabilities_mutex);

	bool cached = g_camera_capabilities_output && g_camera_capabilities_serial &&
		      strcmp(g_camera_capabilities_serial, serial) == 0;
	if (cached && !force) {
		pthread_mutex_unlock(&g_camera_capabilities_mutex);
		return;
	}

	char *output = NULL;
	bool ok = run_scrcpy_camera_query(serial, &output);
	if (ok && output && *output) {
		bfree(g_camera_capabilities_serial);
		bfree(g_camera_capabilities_output);
		g_camera_capabilities_serial = bstrdup(serial);
		g_camera_capabilities_output = bstrdup(output);
	}
	bfree(output);
	pthread_mutex_unlock(&g_camera_capabilities_mutex);
}

static bool refresh_camera_capabilities(obs_properties_t *props, obs_data_t *settings, bool refresh_ids)
{
	obs_property_t *camera_id_prop = obs_properties_get(props, "camera_id");
	obs_property_t *resolution_prop = obs_properties_get(props, "camera_size");
	obs_property_t *fps_prop = obs_properties_get(props, "camera_fps");
	obs_property_t *color_profile_prop = obs_properties_get(props, "camera_color_profile");
	if (!camera_id_prop || !resolution_prop || !fps_prop || !color_profile_prop)
		return false;

	const char *serial = obs_data_get_string(settings, "serial");
	if (!serial || !*serial)
		return false;

	char *camera_output = NULL;
	bool have_query = false;

	pthread_mutex_lock(&g_camera_capabilities_mutex);
	if (g_camera_capabilities_output && g_camera_capabilities_serial &&
	    strcmp(g_camera_capabilities_serial, serial) == 0) {
		camera_output = bstrdup(g_camera_capabilities_output);
	}
	pthread_mutex_unlock(&g_camera_capabilities_mutex);

	if (!camera_output) {
		have_query = run_scrcpy_camera_query(serial, &camera_output);
		if (have_query && camera_output && *camera_output) {
			pthread_mutex_lock(&g_camera_capabilities_mutex);
			bfree(g_camera_capabilities_serial);
			bfree(g_camera_capabilities_output);
			g_camera_capabilities_serial = bstrdup(serial);
			g_camera_capabilities_output = bstrdup(camera_output);
			pthread_mutex_unlock(&g_camera_capabilities_mutex);
		}
	}

	obs_property_list_clear(camera_id_prop);
	obs_property_list_clear(resolution_prop);
	obs_property_list_clear(fps_prop);
	obs_property_list_clear(color_profile_prop);

	if (!camera_output || !*camera_output) {
		populate_camera_fallbacks(camera_id_prop, resolution_prop, fps_prop, color_profile_prop);
		if (camera_output)
			bfree(camera_output);
		return !have_query;
	}

	char selected_id[64] = {0};
	const char *saved_id = obs_data_get_string(settings, "camera_id");
	if (saved_id && *saved_id)
		snprintf(selected_id, sizeof(selected_id), "%s", saved_id);

	int selected_fps[64];
	size_t selected_fps_count = 0;
	float selected_focus_max = 0.0f;
	float selected_zoom_min = 1.0f;
	float selected_zoom_max = 1.0f;
	int selected_wb_min = 0;
	int selected_wb_max = 0;
	bool selected_wb_manual = false;
	bool selected_color_srgb = false;
	bool selected_color_rec709 = false;
	bool selected_color_rec2020 = false;
	bool selected_tone_map_gamma = false;
	bool selected_tone_map_rec709 = false;
	bool selected_tone_map_srgb = false;
	bool selected_tone_map_contrast = false;
	bool selected_tenbit_hlg = false;
	bool selected_tenbit_hdr10 = false;
	bool selected_tenbit_hdr10_plus = false;
	bool first_tenbit_hlg = false;
	bool first_tenbit_hdr10 = false;
	bool first_tenbit_hdr10_plus = false;
	int selected_iso_min = 0;
	int selected_iso_max = 0;
	long long selected_exposure_min_ns = 0;
	long long selected_exposure_max_ns = 0;
	int selected_post_raw_boost_min = 100;
	int selected_post_raw_boost_max = 100;
	int first_iso_min = 0;
	int first_iso_max = 0;
	long long first_exposure_min_ns = 0;
	long long first_exposure_max_ns = 0;
	int first_post_raw_boost_min = 100;
	int first_post_raw_boost_max = 100;
	int first_fps[64];
	size_t first_fps_count = 0;
	size_t camera_count = 0;
	char first_id[64] = {0};
	bool in_selected_camera = false;

	const char *line = camera_output;
	while (line && *line) {
		const char *end = strpbrk(line, "\r\n");
		size_t line_len = end ? (size_t)(end - line) : strlen(line);
		char line_copy[4096];
		if (line_len >= sizeof(line_copy))
			line_len = sizeof(line_copy) - 1;
		memcpy(line_copy, line, line_len);
		line_copy[line_len] = '\0';

		char id[64];
		char label[256];
		int fps[32];
		size_t fps_count = 0;
		float focus_max = 0.0f;
		float zoom_min = 1.0f;
		float zoom_max = 1.0f;
		int wb_min = 0;
		int wb_max = 0;
		bool wb_manual = false;
		bool line_color_srgb = false;
		bool line_color_rec709 = false;
		bool line_color_rec2020 = false;
		bool line_tone_map_gamma = false;
		bool line_tone_map_rec709 = false;
		bool line_tone_map_srgb = false;
		bool line_tone_map_contrast = false;
		bool line_tenbit_hlg = false;
		bool line_tenbit_hdr10 = false;
		bool line_tenbit_hdr10_plus = false;
		parse_camera_color_capabilities(line_copy, &line_color_srgb, &line_color_rec709, &line_color_rec2020,
						&line_tone_map_gamma, &line_tone_map_rec709, &line_tone_map_srgb,
						&line_tone_map_contrast, &line_tenbit_hlg, &line_tenbit_hdr10,
						&line_tenbit_hdr10_plus);
		if (parse_camera_id_line(line_copy, id, sizeof(id), label, sizeof(label), fps, &fps_count, &focus_max,
					 &zoom_min, &zoom_max, &wb_min, &wb_max, &wb_manual)) {
			in_selected_camera = selected_id[0] && strcmp(selected_id, id) == 0;

			int line_iso_min = 0;
			int line_iso_max = 0;
			long long line_exposure_min_ns = 0;
			long long line_exposure_max_ns = 0;
			int line_post_raw_boost_min = 100;
			int line_post_raw_boost_max = 100;
			parse_camera_sensor_ranges(line_copy, &line_iso_min, &line_iso_max, &line_exposure_min_ns,
						   &line_exposure_max_ns);
			parse_camera_post_raw_boost_range(line_copy, &line_post_raw_boost_min,
							  &line_post_raw_boost_max);

			if (!first_id[0]) {
				snprintf(first_id, sizeof(first_id), "%s", id);
				memcpy(first_fps, fps, fps_count * sizeof(int));
				first_fps_count = fps_count;
				first_iso_min = line_iso_min;
				first_iso_max = line_iso_max;
				first_exposure_min_ns = line_exposure_min_ns;
				first_exposure_max_ns = line_exposure_max_ns;
				first_post_raw_boost_min = line_post_raw_boost_min;
				first_post_raw_boost_max = line_post_raw_boost_max;
				first_tenbit_hlg = line_tenbit_hlg;
				first_tenbit_hdr10 = line_tenbit_hdr10;
				first_tenbit_hdr10_plus = line_tenbit_hdr10_plus;
			}
			obs_property_list_add_string(camera_id_prop, label, id);
			camera_count++;
			if (in_selected_camera) {
				for (size_t i = 0; i < fps_count; ++i)
					add_unique_fps(selected_fps, &selected_fps_count, fps[i]);
			}
			if (in_selected_camera && focus_max > 0.0f)
				selected_focus_max = focus_max;
			if (in_selected_camera && zoom_max > zoom_min) {
				selected_zoom_min = zoom_min;
				selected_zoom_max = zoom_max;
			}
			if (in_selected_camera && wb_min > 0 && wb_max > wb_min) {
				selected_wb_min = wb_min;
				selected_wb_max = wb_max;
			}
			if (in_selected_camera && wb_manual)
				selected_wb_manual = true;
			if (in_selected_camera) {
				selected_color_srgb = line_color_srgb;
				selected_color_rec709 = line_color_rec709;
				selected_color_rec2020 = line_color_rec2020;
				selected_tone_map_gamma = line_tone_map_gamma;
				selected_tone_map_rec709 = line_tone_map_rec709;
				selected_tone_map_srgb = line_tone_map_srgb;
				selected_tone_map_contrast = line_tone_map_contrast;
				selected_tenbit_hlg = line_tenbit_hlg;
				selected_tenbit_hdr10 = line_tenbit_hdr10;
				selected_tenbit_hdr10_plus = line_tenbit_hdr10_plus;
			}
			if (in_selected_camera) {
				selected_iso_min = line_iso_min;
				selected_iso_max = line_iso_max;
				selected_exposure_min_ns = line_exposure_min_ns;
				selected_exposure_max_ns = line_exposure_max_ns;
				selected_post_raw_boost_min = line_post_raw_boost_min;
				selected_post_raw_boost_max = line_post_raw_boost_max;
			}
		}

		if (!end)
			break;
		line = end;
		while (*line == '\r' || *line == '\n')
			line++;
	}

	if (!selected_id[0] || camera_count == 0) {
		if (first_id[0]) {
			snprintf(selected_id, sizeof(selected_id), "%s", first_id);
			obs_data_set_string(settings, "camera_id", selected_id);
		}
	}

	if (refresh_ids || obs_property_list_item_count(camera_id_prop) == 0) {
		if (obs_property_list_item_count(camera_id_prop) == 0)
			populate_camera_fallbacks(camera_id_prop, resolution_prop, fps_prop, color_profile_prop);
	}

	if (selected_fps_count == 0 && first_fps_count > 0) {
		memcpy(selected_fps, first_fps, first_fps_count * sizeof(int));
		selected_fps_count = first_fps_count;
	}
	if (selected_iso_max <= 0 && first_iso_max > 0) {
		selected_iso_min = first_iso_min;
		selected_iso_max = first_iso_max;
	}
	if (selected_exposure_max_ns <= 0 && first_exposure_max_ns > 0) {
		selected_exposure_min_ns = first_exposure_min_ns;
		selected_exposure_max_ns = first_exposure_max_ns;
	}
	if (selected_post_raw_boost_max <= 100 && first_post_raw_boost_max > 100) {
		selected_post_raw_boost_min = first_post_raw_boost_min;
		selected_post_raw_boost_max = first_post_raw_boost_max;
	}
	if (!selected_tenbit_hlg && !selected_tenbit_hdr10 && !selected_tenbit_hdr10_plus) {
		selected_tenbit_hlg = first_tenbit_hlg;
		selected_tenbit_hdr10 = first_tenbit_hdr10;
		selected_tenbit_hdr10_plus = first_tenbit_hdr10_plus;
	}
	int saved_fps = (int)obs_data_get_int(settings, "camera_fps");
	if (saved_fps > 0)
		add_unique_fps(selected_fps, &selected_fps_count, saved_fps);
	if (selected_fps_count == 0)
		add_unique_fps(selected_fps, &selected_fps_count, 30);

	for (size_t i = 0; i < selected_fps_count; ++i) {
		for (size_t j = i + 1; j < selected_fps_count; ++j) {
			if (selected_fps[j] < selected_fps[i]) {
				int tmp = selected_fps[i];
				selected_fps[i] = selected_fps[j];
				selected_fps[j] = tmp;
			}
		}
	}
	for (size_t i = 0; i < selected_fps_count; ++i) {
		char label[32];
		snprintf(label, sizeof(label), "%d fps", selected_fps[i]);
		obs_property_list_add_int(fps_prop, label, selected_fps[i]);
	}

	const bool camera_10bit = obs_data_get_bool(settings, "camera_10bit");
	obs_property_list_add_int(color_profile_prop, "Camera-provided / Auto (default)", CAMERA_COLOR_PROFILE_AUTO);
	if (camera_10bit) {

		if (selected_tenbit_hlg)
			obs_property_list_add_int(color_profile_prop, "Rec.2020 / HLG",
						  CAMERA_COLOR_PROFILE_REC2020_HLG);
		if (selected_tenbit_hdr10)
			obs_property_list_add_int(color_profile_prop, "Rec.2020 / PQ (HDR10)",
						  CAMERA_COLOR_PROFILE_REC2020_HDR10);
		if (selected_tenbit_hdr10_plus)
			obs_property_list_add_int(color_profile_prop, "Rec.2020 / PQ (HDR10+)",
						  CAMERA_COLOR_PROFILE_REC2020_HDR10_PLUS);
	}

	if (!camera_10bit && (selected_tone_map_gamma || selected_tone_map_rec709 || selected_tone_map_srgb ||
			      selected_tone_map_contrast)) {
		struct color_profile_option {
			const char *label;
			int value;
			bool color_srgb;
			bool color_rec709;
			bool color_rec2020;
			int gamma;
		} options[] = {
			{"sRGB / Gamma 2.2", CAMERA_COLOR_PROFILE_SRGB_22, true, false, false, CAMERA_GAMMA_22},
			{"sRGB / Gamma 2.4", CAMERA_COLOR_PROFILE_SRGB_24, true, false, false, CAMERA_GAMMA_24},
			{"sRGB / Rec.709 Scene", CAMERA_COLOR_PROFILE_SRGB_REC709_SCENE, true, false, false,
			 CAMERA_GAMMA_REC709_SCENE},
			{"sRGB / Rec.709-A", CAMERA_COLOR_PROFILE_SRGB_REC709_A, true, false, false,
			 CAMERA_GAMMA_REC709_A},
			{"Rec.709 / Gamma 2.2", CAMERA_COLOR_PROFILE_REC709_22, false, true, false, CAMERA_GAMMA_22},
			{"Rec.709 / Gamma 2.4", CAMERA_COLOR_PROFILE_REC709_24, false, true, false, CAMERA_GAMMA_24},
			{"Rec.709 / Rec.709 Scene", CAMERA_COLOR_PROFILE_REC709_REC709_SCENE, false, true, false,
			 CAMERA_GAMMA_REC709_SCENE},
			{"Rec.709 / Rec.709-A", CAMERA_COLOR_PROFILE_REC709_REC709_A, false, true, false,
			 CAMERA_GAMMA_REC709_A},
			{"Rec.2020 / Gamma 2.2", CAMERA_COLOR_PROFILE_REC2020_22, false, false, true, CAMERA_GAMMA_22},
			{"Rec.2020 / Gamma 2.4", CAMERA_COLOR_PROFILE_REC2020_24, false, false, true, CAMERA_GAMMA_24},
			{"Rec.2020 / Rec.709 Scene", CAMERA_COLOR_PROFILE_REC2020_REC709_SCENE, false, false, true,
			 CAMERA_GAMMA_REC709_SCENE},
			{"Rec.2020 / Rec.709-A", CAMERA_COLOR_PROFILE_REC2020_REC709_A, false, false, true,
			 CAMERA_GAMMA_REC709_A},
		};

		for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
			const struct color_profile_option *option = &options[i];
			if (option->gamma == CAMERA_GAMMA_REC709_SCENE && !selected_tone_map_rec709)
				continue;
			if (option->gamma == CAMERA_GAMMA_HLG && !selected_tone_map_contrast)
				continue;
			if (option->gamma != CAMERA_GAMMA_REC709_SCENE && option->gamma != CAMERA_GAMMA_HLG &&
			    !selected_tone_map_gamma)
				continue;
			bool color_supported = (option->color_srgb && selected_color_srgb) ||
					       (option->color_rec709 && selected_color_rec709) ||
					       (option->color_rec2020 && selected_color_rec2020);
			if (color_supported)
				obs_property_list_add_int(color_profile_prop, option->label, option->value);
		}

		if (selected_tone_map_srgb) {
			if (selected_color_srgb)
				obs_property_list_add_int(color_profile_prop, "sRGB / sRGB",
							  CAMERA_COLOR_PROFILE_SRGB_SRGB);
			if (selected_color_rec709)
				obs_property_list_add_int(color_profile_prop, "Rec.709 / sRGB",
							  CAMERA_COLOR_PROFILE_REC709_SRGB);
			if (selected_color_rec2020)
				obs_property_list_add_int(color_profile_prop, "Rec.2020 / sRGB",
							  CAMERA_COLOR_PROFILE_REC2020_SRGB);
		}
		if (selected_tone_map_contrast) {
			if (selected_color_srgb)
				obs_property_list_add_int(color_profile_prop, "sRGB / Linear",
							  CAMERA_COLOR_PROFILE_SRGB_LINEAR);
			if (selected_color_rec709)
				obs_property_list_add_int(color_profile_prop, "Rec.709 / Linear",
							  CAMERA_COLOR_PROFILE_REC709_LINEAR);
			if (selected_color_rec2020)
				obs_property_list_add_int(color_profile_prop, "Rec.2020 / Linear",
							  CAMERA_COLOR_PROFILE_REC2020_LINEAR);
		}
	}

	int saved_color_profile = (int)obs_data_get_int(settings, "camera_color_profile");
	bool color_profile_found = saved_color_profile == CAMERA_COLOR_PROFILE_AUTO;
	for (size_t i = 0; i < obs_property_list_item_count(color_profile_prop); ++i) {
		if (obs_property_list_item_int(color_profile_prop, i) == saved_color_profile) {
			color_profile_found = true;
			break;
		}
	}
	if (!color_profile_found)
		obs_data_set_int(settings, "camera_color_profile", CAMERA_COLOR_PROFILE_AUTO);

	obs_property_t *focus_prop = obs_properties_get(props, "camera_focus_distance");
	if (focus_prop) {
		bool focus_supported = selected_focus_max > 0.0f;
		obs_property_set_enabled(focus_prop, focus_supported);
		obs_property_float_set_limits(focus_prop, 0.0, focus_supported ? selected_focus_max : 1.0, 0.1);
		double current_focus = obs_data_get_double(settings, "camera_focus_distance");
	}

	obs_property_t *zoom_prop = obs_properties_get(props, "camera_zoom");
	if (zoom_prop) {
		float zoom_min = selected_zoom_max > selected_zoom_min ? selected_zoom_min : 1.0f;
		float zoom_max = selected_zoom_max > selected_zoom_min ? selected_zoom_max : 10.0f;
		obs_property_float_set_limits(zoom_prop, zoom_min, zoom_max, 0.05);
	}

	obs_property_t *wb_prop = obs_properties_get(props, "camera_wb_kelvin");
	if (wb_prop) {
		int max_kelvin = selected_wb_max > 0 ? selected_wb_max : 12000;
		obs_property_int_set_limits(wb_prop, 0, max_kelvin, 100);
		obs_property_set_enabled(wb_prop, selected_wb_min > 0 || selected_wb_manual);
	}

	obs_property_t *iso_prop = obs_properties_get(props, "camera_iso");
	if (iso_prop) {
		int sensor_iso_max = selected_iso_max > 0 ? selected_iso_max : 12800;
		int boost_max = selected_post_raw_boost_max > 0 ? selected_post_raw_boost_max : 100;
		long long effective_iso_max_ll = (long long)sensor_iso_max * (long long)boost_max / 100LL;
		int iso_max = effective_iso_max_ll > 1000000LL ? 1000000 : (int)effective_iso_max_ll;
		if (iso_max < sensor_iso_max)
			iso_max = sensor_iso_max;
		obs_property_int_set_limits(iso_prop, 0, iso_max, 1);
		obs_property_set_enabled(iso_prop, true);

		int current_iso = (int)obs_data_get_int(settings, "camera_iso");
		if (current_iso > 0 && selected_iso_min > 0 && current_iso < selected_iso_min)
			obs_data_set_int(settings, "camera_iso", selected_iso_min);
		else if (current_iso > iso_max)
			obs_data_set_int(settings, "camera_iso", iso_max);
	}

	obs_property_t *shutter_prop = obs_properties_get(props, "camera_shutter_us");
	if (shutter_prop)
		populate_shutter_list(shutter_prop, selected_exposure_min_ns, selected_exposure_max_ns);

	parse_selected_camera_sizes(camera_output, selected_id, resolution_prop);
	if (obs_property_list_item_count(resolution_prop) == 0)
		populate_camera_fallbacks(camera_id_prop, resolution_prop, fps_prop, color_profile_prop);

	const char *saved_resolution = obs_data_get_string(settings, "camera_size");
	bool resolution_found = false;
	for (size_t i = 0; i < obs_property_list_item_count(resolution_prop); ++i) {
		const char *value = obs_property_list_item_string(resolution_prop, i);
		if (value && saved_resolution && strcmp(value, saved_resolution) == 0) {
			resolution_found = true;
			break;
		}
	}
	if (!resolution_found && obs_property_list_item_count(resolution_prop) > 0) {
		const char *preferred = "1920x1080";
		bool preferred_found = false;
		for (size_t i = 0; i < obs_property_list_item_count(resolution_prop); ++i) {
			const char *value = obs_property_list_item_string(resolution_prop, i);
			if (value && strcmp(value, preferred) == 0) {
				preferred_found = true;
				break;
			}
		}
		if (!preferred_found)
			preferred = obs_property_list_item_string(resolution_prop, 0);
		obs_data_set_string(settings, "camera_size", preferred);
	}

	if (obs_property_list_item_count(camera_id_prop) > 0 && selected_id[0])
		obs_data_set_string(settings, "camera_id", selected_id);

	bfree(camera_output);
	return true;
}

static void populate_shutter_list(obs_property_t *prop, long long min_exposure_ns, long long max_exposure_ns)
{
	struct shutter_option {
		const char *label;
		int microseconds;
	} options[] = {
		{"8 s", 8000000},   {"4 s", 4000000},  {"2 s", 2000000},   {"1 s", 1000000},   {"1/2 s", 500000},
		{"1/3 s", 333333},  {"1/4 s", 250000}, {"1/5 s", 200000},  {"1/6 s", 166667},  {"1/8 s", 125000},
		{"1/10 s", 100000}, {"1/12 s", 83333}, {"1/15 s", 66667},  {"1/20 s", 50000},  {"1/24 s", 41667},
		{"1/25 s", 40000},  {"1/30 s", 33333}, {"1/40 s", 25000},  {"1/48 s", 20833},  {"1/50 s", 20000},
		{"1/60 s", 16667},  {"1/80 s", 12500}, {"1/96 s", 10417},  {"1/100 s", 10000}, {"1/120 s", 8333},
		{"1/125 s", 8000},  {"1/160 s", 6250}, {"1/180 s", 5556},  {"1/200 s", 5000},  {"1/240 s", 4167},
		{"1/250 s", 4000},  {"1/320 s", 3125}, {"1/400 s", 2500},  {"1/500 s", 2000},  {"1/640 s", 1563},
		{"1/750 s", 1333},  {"1/800 s", 1250}, {"1/1000 s", 1000}, {"1/1250 s", 800},  {"1/1500 s", 667},
		{"1/2000 s", 500},  {"1/2500 s", 400}, {"1/3000 s", 333},  {"1/4000 s", 250},  {"1/5000 s", 200},
		{"1/6000 s", 167},  {"1/8000 s", 125}, {"1/10000 s", 100}, {"1/12000 s", 83},  {"1/16000 s", 63},
		{"1/20000 s", 50},  {"1/32000 s", 31},
	};

	obs_property_list_clear(prop);
	obs_property_list_add_int(prop, "Auto", 0);

	if (min_exposure_ns <= 0 || max_exposure_ns <= 0 || max_exposure_ns < min_exposure_ns) {
		min_exposure_ns = 1000;
		max_exposure_ns = 200000000;
	}

	for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
		const long long exposure_ns = (long long)options[i].microseconds * 1000LL;
		if (exposure_ns < min_exposure_ns || exposure_ns > max_exposure_ns)
			continue;
		obs_property_list_add_int(prop, options[i].label, options[i].microseconds);
	}
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

static bool serial_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(props);
	return true;
}

static bool camera_id_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);

	const char *serial = obs_data_get_string(settings, "serial");
	if (g_camera_capabilities_output && g_camera_capabilities_serial && serial && *serial &&
	    strcmp(g_camera_capabilities_serial, serial) == 0) {
		/* The full camera inventory is cached per device, so switching IDs is local and fast. */
		refresh_camera_capabilities(props, settings, false);
	} else {
		obs_property_t *resolution = obs_properties_get(props, "camera_size");
		obs_property_t *fps = obs_properties_get(props, "camera_fps");
		obs_property_t *camera_id = obs_properties_get(props, "camera_id");
		obs_property_t *color_profile = obs_properties_get(props, "camera_color_profile");
		if (resolution && fps && camera_id && color_profile)
			populate_camera_fallbacks(camera_id, resolution, fps, color_profile);
	}
	return true;
}

static bool camera_fps_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(settings);
	/* Shutter is constrained by the sensor exposure range, not by 1/FPS.
	 * Camera2 may use a longer exposure and a correspondingly longer frame
	 * duration when the device permits it. */
	return true;
}

static bool camera_10bit_supported(const struct scrcpy_src *ctx)
{
	if (!ctx || !ctx->serial || !*ctx->serial)
		return false;

	pthread_mutex_lock(&g_camera_capabilities_mutex);
	bool supported = g_camera_capabilities_output && g_camera_capabilities_serial &&
			 strcmp(g_camera_capabilities_serial, ctx->serial) == 0 &&
			 strstr(g_camera_capabilities_output, "dynamic-range-10bit=true") != NULL &&
			 strstr(g_camera_capabilities_output, "HLG10") != NULL;
	pthread_mutex_unlock(&g_camera_capabilities_mutex);
	return supported;
}

static bool refresh_cameras_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(p);
	struct scrcpy_src *ctx = data;
	if (!ctx || !ctx->source)
		return false;
	obs_data_t *settings = obs_source_get_settings(ctx->source);
	bool ok = refresh_camera_capabilities(props, settings, true);
	obs_data_release(settings);
	obs_property_t *camera_10bit = obs_properties_get(props, "camera_10bit");
	if (camera_10bit)
		obs_property_set_enabled(camera_10bit, camera_10bit_supported(ctx));
	return ok;
}

static bool camera_wb_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);

	/* Changing Kelvin away from Auto must release the native AWB lock, but
	 * this callback must not request a properties-panel refresh while the
	 * slider is being dragged. */
	if (obs_data_get_int(settings, "camera_wb_kelvin") > 0)
		obs_data_set_bool(settings, "camera_wb_lock", false);

	return false;
}

static bool camera_wb_lock_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);

	/* Lock is only meaningful in Auto (0 K). Ignore an attempt to enable it
	 * while a manual Kelvin value is selected, without rebuilding the panel. */
	if (obs_data_get_int(settings, "camera_wb_kelvin") > 0)
		obs_data_set_bool(settings, "camera_wb_lock", false);

	return false;
}

static bool camera_10bit_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	bool enabled = obs_data_get_bool(settings, "camera_10bit");
	if (enabled)
		obs_data_set_string(settings, "codec", "h265");

	/* Rebuild the profile list immediately so it switches between the
	 * 8-bit and 10-bit capability sets without closing the properties dialog. */
	refresh_camera_capabilities(props, settings, false);

	obs_property_t *color_profile = obs_properties_get(props, "camera_color_profile");
	obs_property_t *codec = obs_properties_get(props, "codec");
	if (color_profile)
		obs_property_set_enabled(color_profile, true);
	if (codec)
		obs_property_set_enabled(codec, !enabled);
	return true;
}

static bool video_source_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const char *source = obs_data_get_string(settings, "video_source");
	bool is_camera = source && strcmp(source, "camera") == 0;
	const char *keys[] = {"camera_id",        "camera_size",    "camera_fps",           "camera_zoom",
			      "camera_torch",     "camera_iso",     "camera_shutter_us",    "camera_focus_distance",
			      "camera_wb_kelvin", "camera_wb_lock", "camera_color_profile", "camera_color_range",
			      "camera_10bit",     "portrait_mode",  "flip_vertical",        "hardware_decoding",
			      "refresh_cameras",  "video_buffer_ms"};

	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
		obs_property_t *property = obs_properties_get(props, keys[i]);
		if (property)
			obs_property_set_visible(property, is_camera);
	}

	obs_property_t *max_size = obs_properties_get(props, "max_size");
	if (max_size)
		obs_property_set_visible(max_size, !is_camera);

	return true;
}

static obs_properties_t *src_get_properties(void *data)
{
	struct scrcpy_src *ctx = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *dev_list = obs_properties_add_list(props, "serial", obs_module_text("Device"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_device_list(dev_list);
	if (obs_property_list_item_count(dev_list) == 0) {
		if (ctx->serial && *ctx->serial)
			obs_property_list_add_string(dev_list, ctx->serial, ctx->serial);
		else
			obs_property_list_add_string(dev_list, "No device selected", "");
	}
	obs_property_set_modified_callback(dev_list, serial_modified);

	obs_properties_add_button2(props, "refresh_devices", obs_module_text("RefreshDevices"), refresh_devices_clicked,
				   NULL);

	obs_property_t *src_list = obs_properties_add_list(props, "video_source", obs_module_text("VideoSource"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(src_list, "Display", "display");
	obs_property_list_add_string(src_list, "Camera", "camera");
	obs_property_set_modified_callback(src_list, video_source_modified);

	obs_property_t *camera_id = obs_properties_add_list(props, "camera_id", obs_module_text("CameraId"),
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(camera_id, camera_id_modified);

	obs_property_t *refresh_cameras = obs_properties_add_button2(
		props, "refresh_cameras", obs_module_text("RefreshCameras"), refresh_cameras_clicked, ctx);

	obs_property_t *camera_size = obs_properties_add_list(props, "camera_size", obs_module_text("CameraResolution"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_t *camera_fps = obs_properties_add_list(props, "camera_fps", obs_module_text("CameraFps"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_modified_callback(camera_fps, camera_fps_modified);

	obs_property_t *camera_zoom =
		obs_properties_add_float_slider(props, "camera_zoom", obs_module_text("CameraZoom"), 0.6, 10.0, 0.1);
	obs_property_t *camera_torch = obs_properties_add_bool(props, "camera_torch", obs_module_text("CameraTorch"));
	obs_property_t *camera_iso =
		obs_properties_add_int_slider(props, "camera_iso", obs_module_text("CameraIso"), 0, 12800, 50);
	obs_property_t *camera_shutter = obs_properties_add_list(props, "camera_shutter_us",
								 obs_module_text("CameraShutter"), OBS_COMBO_TYPE_LIST,
								 OBS_COMBO_FORMAT_INT);
	populate_shutter_list(camera_shutter, 1000, 200000000);
	obs_property_t *camera_focus = obs_properties_add_float_slider(props, "camera_focus_distance",
								       obs_module_text("CameraFocus"), 0.0, 20.0, 0.1);
	obs_property_t *camera_wb = obs_properties_add_int_slider(props, "camera_wb_kelvin",
								  obs_module_text("CameraWhiteBalance"), 0, 12000, 100);
	obs_property_set_modified_callback(camera_wb, camera_wb_modified);

	obs_property_t *camera_wb_lock =
		obs_properties_add_bool(props, "camera_wb_lock", obs_module_text("CameraWhiteBalanceLock"));
	obs_property_set_modified_callback(camera_wb_lock, camera_wb_lock_modified);

	obs_property_t *camera_color_profile = obs_properties_add_list(props, "camera_color_profile",
								       obs_module_text("CameraColorProfile"),
								       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(camera_color_profile, "Camera default", CAMERA_COLOR_PROFILE_AUTO);

	obs_property_t *camera_color_range = obs_properties_add_list(props, "camera_color_range",
								     obs_module_text("CameraColorRange"),
								     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(camera_color_range, "Auto (from stream)", SCRCPY_COLOR_RANGE_AUTO);
	obs_property_list_add_int(camera_color_range, "Full range", SCRCPY_COLOR_RANGE_FULL);
	obs_property_list_add_int(camera_color_range, "Limited range", SCRCPY_COLOR_RANGE_LIMITED);

	obs_property_t *camera_10bit = obs_properties_add_bool(props, "camera_10bit", obs_module_text("Camera10Bit"));
	obs_property_set_modified_callback(camera_10bit, camera_10bit_modified);

	obs_property_t *portrait_mode =
		obs_properties_add_bool(props, "portrait_mode", obs_module_text("PortraitMode"));

	obs_property_t *flip_vertical =
		obs_properties_add_bool(props, "flip_vertical", obs_module_text("FlipVertical"));
	obs_property_t *hardware_decoding =
		obs_properties_add_bool(props, "hardware_decoding", obs_module_text("HardwareDecoding"));

	const char *camera_visible = ctx->video_source && strcmp(ctx->video_source, "camera") == 0 ? "camera"
												   : "display";
	bool is_camera = strcmp(camera_visible, "camera") == 0;
	obs_property_t *max_size = obs_properties_get(props, "max_size");
	obs_property_set_visible(camera_id, is_camera);
	if (max_size)
		obs_property_set_visible(max_size, !is_camera);
	obs_property_set_visible(camera_size, is_camera);
	obs_property_set_visible(camera_fps, is_camera);
	obs_property_set_visible(camera_zoom, is_camera);
	obs_property_set_visible(camera_torch, is_camera);
	obs_property_set_visible(camera_iso, is_camera);
	obs_property_set_visible(camera_shutter, is_camera);
	obs_property_set_visible(camera_focus, is_camera);
	obs_property_set_visible(camera_wb, is_camera);
	obs_property_set_visible(camera_wb_lock, is_camera);
	obs_property_set_visible(camera_color_profile, is_camera);
	obs_property_set_visible(camera_color_range, is_camera);
	obs_data_t *ui_settings = obs_source_get_settings(ctx->source);
	bool ten_bit_enabled = obs_data_get_bool(ui_settings, "camera_10bit");
	bool wb_manual = obs_data_get_int(ui_settings, "camera_wb_kelvin") > 0;
	obs_data_release(ui_settings);
	obs_property_set_enabled(camera_wb, true);
	obs_property_set_enabled(camera_wb_lock, !wb_manual);
	obs_property_set_enabled(camera_color_profile, true);
	obs_property_set_visible(camera_10bit, is_camera);
	obs_property_set_visible(portrait_mode, is_camera);
	obs_property_set_visible(refresh_cameras, is_camera);
	obs_property_set_visible(flip_vertical, is_camera);
	obs_property_set_visible(hardware_decoding, is_camera);

	if (ctx->serial && *ctx->serial) {
		pthread_mutex_lock(&g_camera_capabilities_mutex);
		bool have_cached_capabilities = g_camera_capabilities_serial && g_camera_capabilities_output &&
						strcmp(g_camera_capabilities_serial, ctx->serial) == 0;
		pthread_mutex_unlock(&g_camera_capabilities_mutex);

		if (have_cached_capabilities) {
			obs_data_t *settings_now = obs_source_get_settings(ctx->source);
			refresh_camera_capabilities(props, settings_now, false);
			obs_data_release(settings_now);
		} else {
			populate_camera_fallbacks(camera_id, camera_size, camera_fps, camera_color_profile);
		}
	} else {
		populate_camera_fallbacks(camera_id, camera_size, camera_fps, camera_color_profile);
	}

	obs_property_set_enabled(camera_10bit, camera_10bit_supported(ctx));

	obs_property_t *focus_prop = obs_properties_get(props, "camera_focus_distance");
	if (focus_prop) {
		obs_property_set_enabled(focus_prop, true);
		obs_property_float_set_limits(focus_prop, 0.0, 20.0, 0.1);
	}

	max_size = obs_properties_add_int(props, "max_size", obs_module_text("MaxSize"), 0, 4096, 16);
	obs_property_set_visible(max_size, !is_camera);
	obs_properties_add_int(props, "bitrate_kbps", obs_module_text("BitrateKbps"), 500, 50000, 500);

	obs_property_t *codec_list = obs_properties_add_list(props, "codec", obs_module_text("Codec"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(codec_list, "H.264", "h264");
	obs_property_list_add_string(codec_list, "H.265", "h265");
	obs_property_list_add_string(codec_list, "AV1", "av1");

	obs_data_t *codec_settings = obs_source_get_settings(ctx->source);
	bool codec_locked = obs_data_get_bool(codec_settings, "camera_10bit");
	obs_data_release(codec_settings);
	obs_property_set_enabled(codec_list, !codec_locked);

	obs_property_t *buffering = obs_properties_add_list(props, "video_buffer_ms", obs_module_text("Buffering"),
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(buffering, "Automatic (OBS)", 0);
	obs_property_list_add_int(buffering, "50 ms", 50);
	obs_property_list_add_int(buffering, "100 ms", 100);
	obs_property_list_add_int(buffering, "200 ms", 200);
	obs_property_set_visible(buffering, is_camera);

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
