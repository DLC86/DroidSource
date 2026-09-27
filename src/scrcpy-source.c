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

#include <ctype.h>
#include <stdio.h>
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
	char *camera_id;
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
		snprintf(camera_arg, sizeof(camera_arg), "--camera-id=%s", ctx->camera_id ? ctx->camera_id : "0");
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
			snprintf(camera_shutter_arg, sizeof(camera_shutter_arg),
				 "--camera-shutter=%d", ctx->camera_shutter_us);
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
	bfree(ctx->camera_id);
	bfree(ctx->codec);
	bfree(ctx->camera_size);
	bfree(ctx->camera_awb_mode);
	ctx->serial = bstrdup(obs_data_get_string(settings, "serial"));
	ctx->video_source = bstrdup(obs_data_get_string(settings, "video_source"));
	ctx->codec = bstrdup(obs_data_get_string(settings, "codec"));
	bfree(ctx->camera_id);
	const char *camera_id = obs_data_get_string(settings, "camera_id");
	if (camera_id && *camera_id) {
		ctx->camera_id = bstrdup(camera_id);
	} else {
		char camera_id_buf[32];
		snprintf(camera_id_buf, sizeof(camera_id_buf), "%lld", (long long)obs_data_get_int(settings, "camera_id"));
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
	obs_data_set_default_string(settings, "camera_id", "0");
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


static bool run_scrcpy_query(const char *serial, const char *option, char **output)
{
	if (!serial || !*serial)
		return false;

	char *bin_dir = get_bin_dir();
	if (!bin_dir)
		return false;

	char *exe_path = path_join(bin_dir, SCRCPY_EXE_NAME);
	char serial_arg[256];
	snprintf(serial_arg, sizeof(serial_arg), "--serial=%s", serial);

	const char *argv[] = {option, serial_arg, NULL};
	bool ok = scrcpy_proc_run_capture(exe_path, argv, output);

	bfree(exe_path);
	bfree(bin_dir);
	return ok;
}

static bool parse_camera_id_line(const char *line, char *id, size_t id_size, char *label, size_t label_size,
				 int *fps, size_t *fps_count)
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
		snprintf(label, label_size, "Camera %s (%s%s%u%s%u%s)", id, facing,
			 width ? ", " : "", width, width ? "x" : "", height, width ? "" : "");

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

	return true;
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

		char line_copy[1024];
		if (line_len >= sizeof(line_copy))
			line_len = sizeof(line_copy) - 1;
		memcpy(line_copy, line, line_len);
		line_copy[line_len] = '\0';

		char camera_id[64];
		if (parse_camera_id_line(line_copy, camera_id, sizeof(camera_id), NULL, 0, NULL, NULL)) {
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
					if (sscanf(dash + 2, "%ux%u", &width, &height) == 2 && width > 0 && height > 0) {
						char size[32];
						snprintf(size, sizeof(size), "%ux%u", width, height);

						bool duplicate = false;
						for (size_t i = 0; i < count; ++i) {
							const char *existing = obs_property_list_item_string(resolution, i);
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

static bool refresh_camera_capabilities(obs_properties_t *props, obs_data_t *settings, bool refresh_ids)
{
	obs_property_t *camera_id_prop = obs_properties_get(props, "camera_id");
	obs_property_t *resolution_prop = obs_properties_get(props, "camera_size");
	obs_property_t *fps_prop = obs_properties_get(props, "camera_fps");
	if (!camera_id_prop || !resolution_prop || !fps_prop)
		return false;

	const char *serial = obs_data_get_string(settings, "serial");
	if (!serial || !*serial)
		return false;

	char *camera_output = NULL;
	if (!run_scrcpy_query(serial, "--list-cameras", &camera_output))
		return false;

	char selected_id[64] = {0};
	const char *saved_id = obs_data_get_string(settings, "camera_id");
	if (saved_id && *saved_id)
		snprintf(selected_id, sizeof(selected_id), "%s", saved_id);

	int selected_fps[32];
	size_t selected_fps_count = 0;
	size_t camera_count = 0;
	char first_id[64] = {0};

	if (refresh_ids)
		obs_property_list_clear(camera_id_prop);

	const char *line = camera_output;
	while (line && *line) {
		const char *end = strpbrk(line, "\r\n");
		size_t line_len = end ? (size_t)(end - line) : strlen(line);
		char line_copy[1024];
		if (line_len >= sizeof(line_copy))
			line_len = sizeof(line_copy) - 1;
		memcpy(line_copy, line, line_len);
		line_copy[line_len] = '\0';

		char id[64];
		char label[256];
		int fps[32];
		size_t fps_count = 0;
		if (parse_camera_id_line(line_copy, id, sizeof(id), label, sizeof(label), fps, &fps_count)) {
			if (!first_id[0])
				snprintf(first_id, sizeof(first_id), "%s", id);

			if (refresh_ids)
				obs_property_list_add_string(camera_id_prop, label, id);

			if (selected_id[0] && strcmp(selected_id, id) == 0) {
				memcpy(selected_fps, fps, fps_count * sizeof(int));
				selected_fps_count = fps_count;
			}
			camera_count++;
		}

		if (!end)
			break;
		line = end;
		while (*line == '\r' || *line == '\n')
			line++;
	}
	bfree(camera_output);

	if (!selected_id[0] || (refresh_ids && camera_count > 0)) {
		bool found = false;
		for (size_t i = 0; i < obs_property_list_item_count(camera_id_prop); ++i) {
			const char *value = obs_property_list_item_string(camera_id_prop, i);
			if (value && strcmp(value, selected_id) == 0) {
				found = true;
				break;
			}
		}
		if (!found && first_id[0]) {
			snprintf(selected_id, sizeof(selected_id), "%s", first_id);
			obs_data_set_string(settings, "camera_id", selected_id);
		}
	}

	obs_property_list_clear(fps_prop);
	if (selected_fps_count == 0)
		selected_fps[0] = 30, selected_fps_count = 1;
	for (size_t i = 0; i < selected_fps_count; ++i) {
		char label[32];
		snprintf(label, sizeof(label), "%d fps", selected_fps[i]);
		obs_property_list_add_int(fps_prop, label, selected_fps[i]);
	}

	char *size_output = NULL;
	bool have_sizes = run_scrcpy_query(serial, "--list-camera-sizes", &size_output);
	obs_property_list_clear(resolution_prop);
	bool have_selected_sizes = false;
	if (have_sizes) {
		have_selected_sizes = parse_selected_camera_sizes(size_output, selected_id, resolution_prop);
		bfree(size_output);
	}

	if (!have_selected_sizes) {
		obs_property_list_add_string(resolution_prop, "1920x1080", "1920x1080");
	}

	const char *saved_resolution = obs_data_get_string(settings, "camera_size");
	bool resolution_found = false;
	for (size_t i = 0; i < obs_property_list_item_count(resolution_prop); ++i) {
		const char *value = obs_property_list_item_string(resolution_prop, i);
		if (value && saved_resolution && strcmp(value, saved_resolution) == 0) {
			resolution_found = true;
			break;
		}
	}
	if (!resolution_found) {
		const char *preferred = "1920x1080";
		bool preferred_found = false;
		for (size_t i = 0; i < obs_property_list_item_count(resolution_prop); ++i) {
			const char *value = obs_property_list_item_string(resolution_prop, i);
			if (value && strcmp(value, preferred) == 0) {
				preferred_found = true;
				break;
			}
		}
		if (!preferred_found && obs_property_list_item_count(resolution_prop) > 0)
			preferred = obs_property_list_item_string(resolution_prop, 0);
		obs_data_set_string(settings, "camera_size", preferred);
	}

	if (obs_property_list_item_count(camera_id_prop) > 0) {
		obs_data_set_string(settings, "camera_id", selected_id);
	}
	return true;
}

static void populate_shutter_list(obs_property_t *prop, int fps)
{
	struct shutter_option {
		int denominator;
		int microseconds;
	} options[] = {
		{5, 200000}, {6, 166667}, {8, 125000}, {10, 100000}, {12, 83333}, {15, 66667},
		{20, 50000}, {24, 41667}, {25, 40000}, {30, 33333}, {40, 25000}, {48, 20833},
		{50, 20000}, {60, 16667}, {80, 12500}, {96, 10417}, {100, 10000}, {120, 8333},
		{125, 8000}, {160, 6250}, {180, 5556}, {200, 5000}, {240, 4167}, {250, 4000},
		{320, 3125}, {400, 2500}, {500, 2000}, {640, 1563}, {750, 1333}, {800, 1250},
		{1000, 1000}, {1250, 800}, {1500, 667}, {2000, 500}, {2500, 400}, {3000, 333},
		{4000, 250}, {5000, 200}, {6000, 167}, {8000, 125}, {10000, 100}, {12000, 83},
		{16000, 63}, {20000, 50}, {32000, 31},
	};

	obs_property_list_clear(prop);
	obs_property_list_add_int(prop, "Auto", 0);
	if (fps < 1)
		fps = 30;

	for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
		if (options[i].denominator >= fps) {
			char label[32];
			snprintf(label, sizeof(label), "1/%d s", options[i].denominator);
			obs_property_list_add_int(prop, label, options[i].microseconds);
		}
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
	if (obs_data_get_string(settings, "video_source") && strcmp(obs_data_get_string(settings, "video_source"), "camera") == 0)
		refresh_camera_capabilities(props, settings, true);
	return true;
}

static bool camera_id_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	refresh_camera_capabilities(props, settings, false);
	return true;
}

static bool camera_fps_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	obs_property_t *shutter = obs_properties_get(props, "camera_shutter_us");
	if (shutter)
		populate_shutter_list(shutter, (int)obs_data_get_int(settings, "camera_fps"));
	return true;
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
	return ok;
}

static bool video_source_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const char *source = obs_data_get_string(settings, "video_source");
	bool is_camera = source && strcmp(source, "camera") == 0;
	obs_property_t *camera_id = obs_properties_get(props, "camera_id");
	if (camera_id)
		obs_property_set_visible(camera_id, is_camera);
	const char *keys[] = {"camera_size", "camera_fps", "camera_zoom", "camera_torch", "camera_iso",
			      "camera_shutter_us", "camera_focus_distance", "camera_awb_mode"};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
		obs_property_t *prop = obs_properties_get(props, keys[i]);
		if (prop)
			obs_property_set_visible(prop, is_camera);
	}
	if (is_camera)
		refresh_camera_capabilities(props, settings, true);
	return true;
}

static obs_properties_t *src_get_properties(void *data)
{
	struct scrcpy_src *ctx = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *dev_list = obs_properties_add_list(props, "serial", obs_module_text("Device"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_device_list(dev_list);
	obs_property_set_modified_callback(dev_list, serial_modified);

	obs_properties_add_button2(props, "refresh_devices", obs_module_text("RefreshDevices"), refresh_devices_clicked, NULL);

	obs_property_t *src_list = obs_properties_add_list(props, "video_source", obs_module_text("VideoSource"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(src_list, "Display", "display");
	obs_property_list_add_string(src_list, "Camera", "camera");
	obs_property_set_modified_callback(src_list, video_source_modified);

	obs_property_t *camera_id = obs_properties_add_list(props, "camera_id", obs_module_text("CameraId"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(camera_id, camera_id_modified);

	obs_property_t *camera_size = obs_properties_add_list(props, "camera_size", obs_module_text("CameraResolution"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_t *camera_fps = obs_properties_add_list(props, "camera_fps", obs_module_text("CameraFps"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_modified_callback(camera_fps, camera_fps_modified);

	obs_property_t *camera_zoom = obs_properties_add_float_slider(props, "camera_zoom", obs_module_text("CameraZoom"), 1.0, 10.0, 0.1);
	obs_property_t *camera_torch = obs_properties_add_bool(props, "camera_torch", obs_module_text("CameraTorch"));
	obs_property_t *camera_iso = obs_properties_add_int_slider(props, "camera_iso", obs_module_text("CameraIso"), 0, 12800, 50);
	obs_property_t *camera_shutter = obs_properties_add_list(props, "camera_shutter_us", obs_module_text("CameraShutter"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_data_t *current_settings = obs_source_get_settings(ctx->source);
	populate_shutter_list(camera_shutter, (int)obs_data_get_int(current_settings, "camera_fps"));
	obs_data_release(current_settings);
	obs_property_t *camera_focus = obs_properties_add_float_slider(props, "camera_focus_distance", obs_module_text("CameraFocus"),
							   0.0, 20.0, 0.1);
	obs_property_t *camera_awb = obs_properties_add_list(props, "camera_awb_mode", obs_module_text("CameraWhiteBalance"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(camera_awb, "Auto", "auto");
	obs_property_list_add_string(camera_awb, "Incandescent", "incandescent");
	obs_property_list_add_string(camera_awb, "Fluorescent", "fluorescent");
	obs_property_list_add_string(camera_awb, "Daylight", "daylight");
	obs_property_list_add_string(camera_awb, "Cloudy", "cloudy");

	obs_properties_add_button2(props, "refresh_cameras", obs_module_text("RefreshCameras"), refresh_cameras_clicked, ctx);

	const char *camera_visible = ctx->video_source && strcmp(ctx->video_source, "camera") == 0 ? "camera" : "display";
	bool is_camera = strcmp(camera_visible, "camera") == 0;
	obs_property_set_visible(camera_id, is_camera);
	obs_property_set_visible(camera_size, is_camera);
	obs_property_set_visible(camera_fps, is_camera);
	obs_property_set_visible(camera_zoom, is_camera);
	obs_property_set_visible(camera_torch, is_camera);
	obs_property_set_visible(camera_iso, is_camera);
	obs_property_set_visible(camera_shutter, is_camera);
	obs_property_set_visible(camera_focus, is_camera);
	obs_property_set_visible(camera_awb, is_camera);

	if (is_camera) {
		obs_data_t *settings = obs_source_get_settings(ctx->source);
		refresh_camera_capabilities(props, settings, true);
		obs_data_release(settings);
	}

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
