#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "scrcpy-reader.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>

#include <stdint.h>
#include <string.h>
#include <pthread.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#endif

#define SC_PACKET_FLAG_CONFIG    (UINT64_C(1) << 63)
#define SC_PACKET_FLAG_KEY_FRAME (UINT64_C(1) << 62)
#define SC_PACKET_PTS_MASK       (SC_PACKET_FLAG_KEY_FRAME - 1)

#define SC_CODEC_ID_H264 UINT32_C(0x68323634)
#define SC_CODEC_ID_H265 UINT32_C(0x68323635)
#define SC_CODEC_ID_AV1  UINT32_C(0x00617631)

#ifdef _WIN32
typedef SOCKET sock_t;
#define INVALID_SOCK INVALID_SOCKET
#define SOCK_ERR (-1)
static void close_sock(sock_t s)
{
	if (s != INVALID_SOCK)
		closesocket(s);
}
#else
typedef int sock_t;
#define INVALID_SOCK (-1)
#define SOCK_ERR (-1)
static void close_sock(sock_t s)
{
	if (s >= 0)
		close(s);
}
#endif

struct scrcpy_reader {
	obs_source_t *source;
	uint16_t port;

	pthread_t thread;
	bool thread_started;
	volatile bool stop;
	volatile bool running;

	pthread_mutex_t state_mutex;
	uint64_t last_frame_ns;

	sock_t sock;

	bool hardware_decoding; /* clang-format sync */
	bool flip_vertical;
	int video_buffer_ms;
	bool portrait_mode;
	int color_range_override;
	bool logged_color_info;

	AVBufferRef *hw_device_ctx;
	enum AVPixelFormat hw_pix_fmt;
	AVFrame *transfer_frame;
	AVFrame *portrait_frame;

	AVCodecContext *codec_ctx;
	AVPacket *packet;
	AVFrame *frame;

	/* Pending config (SPS/PPS) to be prepended to next media packet.
	 * Mirrors scrcpy's sc_packet_merger behavior. */
	uint8_t *pending_config;
	size_t pending_config_size;
};

static uint32_t rd_u32be(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd_u64be(const uint8_t *p)
{
	return ((uint64_t)rd_u32be(p) << 32) | (uint64_t)rd_u32be(p + 4);
}

static bool recv_all(sock_t s, void *buf, size_t len, volatile bool *stop)
{
	uint8_t *p = buf;
	while (len > 0) {
		if (os_atomic_load_bool(stop))
			return false;
#ifdef _WIN32
		int r = recv(s, (char *)p, (int)len, 0);
#else
		ssize_t r = recv(s, p, len, 0);
#endif
		if (r <= 0)
			return false;
		p += r;
		len -= (size_t)r;
	}
	return true;
}

static size_t socket_pending_bytes(sock_t s)
{
#ifdef _WIN32
	u_long pending = 0;
	if (ioctlsocket(s, FIONREAD, &pending) != 0)
		return 0;
	return (size_t)pending;
#else
	int pending = 0;
	if (ioctl(s, FIONREAD, &pending) != 0 || pending <= 0)
		return 0;
	return (size_t)pending;
#endif
}

static bool discard_all(sock_t s, size_t len, volatile bool *stop)
{
	uint8_t buffer[65536];
	while (len > 0) {
		size_t chunk = len < sizeof(buffer) ? len : sizeof(buffer);
		if (!recv_all(s, buffer, chunk, stop))
			return false;
		len -= chunk;
	}
	return true;
}

static enum AVCodecID scrcpy_codec_to_avcodec(uint32_t id)
{
	switch (id) {
	case SC_CODEC_ID_H264:
		return AV_CODEC_ID_H264;
	case SC_CODEC_ID_H265:
		return AV_CODEC_ID_HEVC;
	case SC_CODEC_ID_AV1:
		return AV_CODEC_ID_AV1;
	default:
		return AV_CODEC_ID_NONE;
	}
}

static enum video_format av_to_obs_format(enum AVPixelFormat pix)
{
	switch (pix) {
	case AV_PIX_FMT_YUV420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_YUV422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P:
		return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUV420P10LE:
		return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_P010LE:
		return VIDEO_FORMAT_P010;
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

static sock_t connect_with_retry(uint16_t port, volatile bool *stop)
{
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);

	for (int i = 0; i < 100; ++i) {
		if (os_atomic_load_bool(stop))
			return INVALID_SOCK;

		sock_t s = socket(AF_INET, SOCK_STREAM, 0);
		if (s == INVALID_SOCK)
			return INVALID_SOCK;

		if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			int one = 1;
			setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
			return s;
		}
		close_sock(s);
		os_sleep_ms(100);
	}
	return INVALID_SOCK;
}

#ifdef _WIN32
static enum AVPixelFormat get_hw_format(AVCodecContext *codec_ctx, const enum AVPixelFormat *pix_fmts)
{
	struct scrcpy_reader *r = codec_ctx->opaque;
	if (r) {
		for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
			if (*p == r->hw_pix_fmt)
				return *p;
		}
	}
	return pix_fmts[0];
}
#endif

static enum video_colorspace obs_colorspace_from_av(const AVFrame *frame)
{
	switch ((enum AVColorSpace)frame->colorspace) {
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_BT470BG:
		return VIDEO_CS_601;
	case AVCOL_SPC_BT709:
		return VIDEO_CS_709;
	case AVCOL_SPC_BT2020_NCL:
	case AVCOL_SPC_BT2020_CL:
		if (frame->color_trc == AVCOL_TRC_ARIB_STD_B67)
			return VIDEO_CS_2100_HLG;
		if (frame->color_trc == AVCOL_TRC_SMPTE2084)
			return VIDEO_CS_2100_PQ;
		/*
		 * OBS 32.2.x has no standalone BT.2020-SDR enum. The caller
		 * therefore applies the BT.2020 matrix explicitly below.
		 */
		return VIDEO_CS_709;
	default:
		return VIDEO_CS_709;
	}
}

static enum video_range_type obs_range_from_av(const AVFrame *frame)
{
	return frame->color_range == AVCOL_RANGE_JPEG ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
}

static enum video_range_type resolve_color_range(const AVFrame *frame, int override)
{
	enum video_range_type range = obs_range_from_av(frame);

	if (override == SCRCPY_COLOR_RANGE_FULL)
		return VIDEO_RANGE_FULL;
	if (override == SCRCPY_COLOR_RANGE_LIMITED)
		return VIDEO_RANGE_PARTIAL;
	return range;
}

static bool obs_bt2020_sdr_matrix(enum video_format format, enum video_range_type range, float matrix[16],
				  float min_range[3], float max_range[3])
{
	/*
	 * Match libobs 32.2.x video-matrices.c, but use BT.2020-NCL
	 * coefficients (Kb=0.0593, Kr=0.2627) for BT.2020 SDR.
	 */
	const float kb = 0.0593f;
	const float kr = 0.2627f;
	const float kg = 1.0f - kb - kr;
	uint32_t bpc = 8;

	switch (format) {
	case VIDEO_FORMAT_I010:
	case VIDEO_FORMAT_P010:
	case VIDEO_FORMAT_I210:
	case VIDEO_FORMAT_V210:
	case VIDEO_FORMAT_R10L:
		bpc = 10;
		break;
	case VIDEO_FORMAT_I412:
	case VIDEO_FORMAT_YA2L:
		bpc = 12;
		break;
	case VIDEO_FORMAT_P216:
	case VIDEO_FORMAT_P416:
		bpc = 16;
		break;
	default:
		break;
	}

	const float bit_max = (float)((UINT32_C(1) << bpc) - 1U);
	const float scale = (float)(UINT32_C(1) << (bpc - 8));
	const bool full_range = range == VIDEO_RANGE_FULL;

	const float y_min = full_range ? 0.0f : 16.0f * scale;
	const float y_max = full_range ? bit_max : 235.0f * scale;
	const float uv_min = full_range ? 0.0f : 16.0f * scale;
	const float uv_max = full_range ? bit_max : 240.0f * scale;
	const float black_y = full_range ? 0.0f : 16.0f * scale;
	const float black_uv = full_range ? 0.5f * bit_max : 128.0f * scale;

	const float yscale = bit_max / (y_max - y_min);
	const float uscale = bit_max / ((uv_max - uv_min) / 2.0f);
	const float vscale = uscale;

	const float m00 = yscale;
	const float m01 = 0.0f;
	const float m02 = vscale * (1.0f - kr);
	const float m10 = yscale;
	const float m11 = uscale * (kb - 1.0f) * kb / kg;
	const float m12 = vscale * (kr - 1.0f) * kr / kg;
	const float m20 = yscale;
	const float m21 = uscale * (1.0f - kb);
	const float m22 = 0.0f;

	const float y_off = -black_y / bit_max;
	const float u_off = -black_uv / bit_max;
	const float v_off = -black_uv / bit_max;

	matrix[0] = m00;
	matrix[1] = m01;
	matrix[2] = m02;
	matrix[3] = m00 * y_off + m01 * u_off + m02 * v_off;
	matrix[4] = m10;
	matrix[5] = m11;
	matrix[6] = m12;
	matrix[7] = m10 * y_off + m11 * u_off + m12 * v_off;
	matrix[8] = m20;
	matrix[9] = m21;
	matrix[10] = m22;
	matrix[11] = m20 * y_off + m21 * u_off + m22 * v_off;
	matrix[12] = 0.0f;
	matrix[13] = 0.0f;
	matrix[14] = 0.0f;
	matrix[15] = 1.0f;

	if (min_range) {
		const float minv = full_range ? 0.0f : 16.0f / 255.0f;
		min_range[0] = minv;
		min_range[1] = minv;
		min_range[2] = minv;
	}
	if (max_range) {
		const float maxv = full_range ? 1.0f : 235.0f / 255.0f;
		max_range[0] = maxv;
		max_range[1] = maxv;
		max_range[2] = maxv;
	}

	return true;
}

static uint8_t obs_trc_from_av(const AVFrame *frame)
{
	if (frame->color_trc == AVCOL_TRC_ARIB_STD_B67)
		return VIDEO_TRC_HLG;
	if (frame->color_trc == AVCOL_TRC_SMPTE2084)
		return VIDEO_TRC_PQ;
	return VIDEO_TRC_SRGB;
}

static void log_frame_color_info(struct scrcpy_reader *r, const AVFrame *frame, bool hardware_path)
{
	if (r->logged_color_info || !frame)
		return;

	obs_log(LOG_INFO,
		"scrcpy-reader: decoded color metadata: path=%s format=%d "
		"colorspace=%d primaries=%d transfer=%d range=%d",
		hardware_path ? "D3D11VA" : "software", frame->format, frame->colorspace, frame->color_primaries,
		frame->color_trc, frame->color_range);
	r->logged_color_info = true;
}

static void rotate_plane_90_ccw(uint8_t *dst, int dst_linesize, const uint8_t *src, int src_linesize, int src_width,
				int src_height, int bytes_per_pixel)
{
	for (int sy = 0; sy < src_height; ++sy) {
		for (int sx = 0; sx < src_width; ++sx) {
			int dx = sy;
			int dy = src_width - 1 - sx;
			memcpy(dst + (size_t)dy * dst_linesize + (size_t)dx * bytes_per_pixel,
			       src + (size_t)sy * src_linesize + (size_t)sx * bytes_per_pixel, (size_t)bytes_per_pixel);
		}
	}
}

static bool rotate_frame_90_ccw(struct scrcpy_reader *r, const AVFrame *src)
{
	enum AVPixelFormat format;

	if (!r || !src || !r->portrait_frame)
		return false;

	format = (enum AVPixelFormat)src->format;
	bool high_bit_depth = format == AV_PIX_FMT_YUV420P10LE || format == AV_PIX_FMT_P010LE;
	if (format != AV_PIX_FMT_YUV420P && format != AV_PIX_FMT_YUVJ420P && format != AV_PIX_FMT_NV12 &&
	    !high_bit_depth)
		return false;

	av_frame_unref(r->portrait_frame);
	r->portrait_frame->format = src->format;
	r->portrait_frame->width = src->height;
	r->portrait_frame->height = src->width;
	if (av_frame_copy_props(r->portrait_frame, src) < 0)
		return false;
	if (av_frame_get_buffer(r->portrait_frame, 32) < 0)
		return false;

	int bytes_per_luma = high_bit_depth ? 2 : 1;
	rotate_plane_90_ccw(r->portrait_frame->data[0], r->portrait_frame->linesize[0], src->data[0], src->linesize[0],
			    src->width, src->height, bytes_per_luma);

	int src_width = src->width / 2;
	int src_height = src->height / 2;
	if (format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_P010LE) {
		rotate_plane_90_ccw(r->portrait_frame->data[1], r->portrait_frame->linesize[1], src->data[1],
				    src->linesize[1], src_width, src_height, format == AV_PIX_FMT_P010LE ? 4 : 2);
	} else {
		rotate_plane_90_ccw(r->portrait_frame->data[1], r->portrait_frame->linesize[1], src->data[1],
				    src->linesize[1], src_width, src_height, bytes_per_luma);
		rotate_plane_90_ccw(r->portrait_frame->data[2], r->portrait_frame->linesize[2], src->data[2],
				    src->linesize[2], src_width, src_height, bytes_per_luma);
	}

	return true;
}

static bool open_decoder(struct scrcpy_reader *r, uint32_t codec_id, uint32_t width, uint32_t height)
{
	enum AVCodecID av_id = scrcpy_codec_to_avcodec(codec_id);
	if (av_id == AV_CODEC_ID_NONE) {
		obs_log(LOG_ERROR, "scrcpy-reader: unsupported codec 0x%08x", codec_id);
		return false;
	}
	const AVCodec *codec = avcodec_find_decoder(av_id);
	if (!codec) {
		obs_log(LOG_ERROR, "scrcpy-reader: no decoder for codec id %d", (int)av_id);
		return false;
	}
	r->codec_ctx = avcodec_alloc_context3(codec);
	if (!r->codec_ctx)
		return false;

	r->codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	r->codec_ctx->opaque = r;

#ifdef _WIN32
	if (r->hardware_decoding) {
		for (int i = 0;; ++i) {
			const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
			if (!config)
				break;
			if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
			    config->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
				r->hw_pix_fmt = config->pix_fmt;
				break;
			}
		}
		if (r->hw_pix_fmt != AV_PIX_FMT_NONE) {
			if (av_hwdevice_ctx_create(&r->hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0) == 0) {
				r->codec_ctx->get_format = get_hw_format;
				r->codec_ctx->hw_device_ctx = av_buffer_ref(r->hw_device_ctx);
				obs_log(LOG_INFO, "scrcpy-reader: using D3D11VA hardware decoding");
			} else {
				r->hw_pix_fmt = AV_PIX_FMT_NONE;
				obs_log(LOG_INFO, "scrcpy-reader: D3D11VA unavailable; using software decoding");
			}
		}
	}
#endif

	r->codec_ctx->thread_type = FF_THREAD_SLICE;
	if (codec_id == SC_CODEC_ID_H265)
		r->codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
	/* Slice threading avoids frame-thread buffering; let FFmpeg choose
	 * the available slice threads so 4K HEVC does not become CPU-starved. */
	r->codec_ctx->thread_count = 0;
	r->codec_ctx->width = (int)width;
	r->codec_ctx->height = (int)height;
	/* Let FFmpeg choose the native decoded pixel format, including 10-bit formats. */

	if (avcodec_open2(r->codec_ctx, codec, NULL) < 0) {
		obs_log(LOG_ERROR, "scrcpy-reader: avcodec_open2 failed");
		avcodec_free_context(&r->codec_ctx);
		return false;
	}

	r->packet = av_packet_alloc();
	r->frame = av_frame_alloc();
	r->transfer_frame = av_frame_alloc();
	r->portrait_frame = av_frame_alloc();
	if (!r->packet || !r->frame || !r->transfer_frame || !r->portrait_frame) {
		obs_log(LOG_ERROR, "scrcpy-reader: av alloc failed");
		return false;
	}
	return true;
}

static void emit_frame(struct scrcpy_reader *r, AVFrame *f)
{
	AVFrame *out = f;
	bool hardware_path = false;

	if (r->hw_pix_fmt != AV_PIX_FMT_NONE && f->format == r->hw_pix_fmt) {
		hardware_path = true;
		av_frame_unref(r->transfer_frame);
		if (av_hwframe_transfer_data(r->transfer_frame, f, 0) < 0) {
			obs_log(LOG_WARNING, "scrcpy-reader: hardware frame transfer failed");
			return;
		}
		/*
		 * The generic hardware->software transfer copies the pixel data but
		 * does not guarantee AVFrame color properties are preserved. Restore
		 * the exact decoder metadata before passing the frame to OBS.
		 */
		if (av_frame_copy_props(r->transfer_frame, f) < 0) {
			obs_log(LOG_WARNING, "scrcpy-reader: could not preserve hardware frame color metadata");
			return;
		}
		out = r->transfer_frame;
	}

	log_frame_color_info(r, out, hardware_path);

	if (r->portrait_mode) {
		if (!rotate_frame_90_ccw(r, out)) {
			obs_log(LOG_WARNING, "scrcpy-reader: portrait mode unsupported for pixel format %d",
				out->format);
			return;
		}
		out = r->portrait_frame;
	}

	enum video_format fmt = av_to_obs_format((enum AVPixelFormat)out->format);
	if (fmt == VIDEO_FORMAT_NONE) {
		obs_log(LOG_WARNING, "scrcpy-reader: unsupported pix fmt %d", out->format);
		return;
	}

	struct obs_source_frame obs_frame = {0};
	obs_frame.format = fmt;
	obs_frame.width = (uint32_t)out->width;
	obs_frame.height = (uint32_t)out->height;
	for (int i = 0; i < 4; ++i) {
		obs_frame.data[i] = out->data[i];
		obs_frame.linesize[i] = (uint32_t)out->linesize[i];
	}
	if (out->pts == AV_NOPTS_VALUE)
		obs_frame.timestamp = (uint64_t)os_gettime_ns();
	else
		obs_frame.timestamp = (uint64_t)out->pts * 1000ULL;
	obs_frame.timestamp += (uint64_t)r->video_buffer_ms * UINT64_C(1000000);
	obs_frame.flip = r->flip_vertical;

	pthread_mutex_lock(&r->state_mutex);
	r->last_frame_ns = os_gettime_ns();
	int color_range_override = r->color_range_override;
	pthread_mutex_unlock(&r->state_mutex);

	enum video_colorspace cs = obs_colorspace_from_av(out);
	enum video_range_type range = resolve_color_range(out, color_range_override);
	bool matrix_ok;
	const bool bt2020_sdr = ((enum AVColorSpace)out->colorspace == AVCOL_SPC_BT2020_NCL ||
				 (enum AVColorSpace)out->colorspace == AVCOL_SPC_BT2020_CL) &&
				out->color_trc != AVCOL_TRC_ARIB_STD_B67 && out->color_trc != AVCOL_TRC_SMPTE2084;

	if (bt2020_sdr) {
		matrix_ok = obs_bt2020_sdr_matrix(fmt, range, obs_frame.color_matrix, obs_frame.color_range_min,
						  obs_frame.color_range_max);
		obs_log(LOG_DEBUG, "scrcpy-reader: using explicit BT.2020-SDR YUV matrix");
	} else {
		matrix_ok = video_format_get_parameters_for_format(
			cs, range, fmt, obs_frame.color_matrix, obs_frame.color_range_min, obs_frame.color_range_max);
	}

	if (!matrix_ok) {
		obs_log(LOG_WARNING, "scrcpy-reader: could not build color matrix for colorspace=%d format=%d range=%d",
			out->colorspace, fmt, range);
		return;
	}
	obs_frame.full_range = range == VIDEO_RANGE_FULL;
	obs_frame.trc = obs_trc_from_av(out);

	obs_source_output_video(r->source, &obs_frame);
}

static void *reader_thread(void *data)
{
	struct scrcpy_reader *r = data;

	os_set_thread_name("scrcpy-reader");
	os_atomic_set_bool(&r->running, true);
	pthread_mutex_lock(&r->state_mutex);
	r->last_frame_ns = os_gettime_ns();
	pthread_mutex_unlock(&r->state_mutex);

	r->sock = connect_with_retry(r->port, &r->stop);
	if (r->sock == INVALID_SOCK) {
		obs_log(LOG_ERROR, "scrcpy-reader: could not connect to 127.0.0.1:%u", (unsigned)r->port);
		os_atomic_set_bool(&r->running, false);
		return NULL;
	}
	obs_log(LOG_INFO, "scrcpy-reader: connected to 127.0.0.1:%u, awaiting prelude", (unsigned)r->port);

	uint8_t prelude[12];
	if (!recv_all(r->sock, prelude, sizeof(prelude), &r->stop)) {
		obs_log(LOG_ERROR, "scrcpy-reader: prelude read failed");
		goto done;
	}

	uint32_t codec_id = rd_u32be(prelude);
	uint32_t width = rd_u32be(prelude + 4);
	uint32_t height = rd_u32be(prelude + 8);
	obs_log(LOG_INFO, "scrcpy-reader: prelude codec=0x%08x %ux%u", codec_id, width, height);

	if (!open_decoder(r, codec_id, width, height))
		goto done;

	bool drop_until_keyframe = false;

	while (!os_atomic_load_bool(&r->stop)) {
		size_t backlog_limit = codec_id == SC_CODEC_ID_H265 ? (256 * 1024) : (512 * 1024);
		if (!drop_until_keyframe && socket_pending_bytes(r->sock) > backlog_limit) {
			obs_log(LOG_WARNING, "scrcpy-reader: video backlog exceeded %zu KiB; dropping to next keyframe",
				backlog_limit / 1024);
			drop_until_keyframe = true;
			avcodec_flush_buffers(r->codec_ctx);
			bfree(r->pending_config);
			r->pending_config = NULL;
			r->pending_config_size = 0;
		}

		uint8_t hdr[12];
		if (!recv_all(r->sock, hdr, sizeof(hdr), &r->stop))
			break;

		uint64_t pts_flags = rd_u64be(hdr);
		uint32_t size = rd_u32be(hdr + 8);
		if (size == 0)
			continue;

		bool is_config = (pts_flags & SC_PACKET_FLAG_CONFIG) != 0;
		bool is_key = (pts_flags & SC_PACKET_FLAG_KEY_FRAME) != 0;

		if (is_config) {
			/* Buffer SPS/PPS (or VPS+SPS+PPS) — prepend to next
			 * media packet. scrcpy emits one config packet before
			 * every IDR. */
			uint8_t *buf = bmalloc(size);
			if (!recv_all(r->sock, buf, size, &r->stop)) {
				bfree(buf);
				break;
			}
			bfree(r->pending_config);
			r->pending_config = buf;
			r->pending_config_size = size;
			continue;
		}

		if (drop_until_keyframe && !is_key) {
			if (!discard_all(r->sock, size, &r->stop))
				break;
			continue;
		}

		if (drop_until_keyframe && is_key)
			drop_until_keyframe = false;

		size_t cfg = r->pending_config ? r->pending_config_size : 0;
		if (av_new_packet(r->packet, (int)(cfg + size)) != 0) {
			obs_log(LOG_ERROR, "scrcpy-reader: av_new_packet OOM");
			break;
		}
		if (cfg) {
			memcpy(r->packet->data, r->pending_config, cfg);
			bfree(r->pending_config);
			r->pending_config = NULL;
			r->pending_config_size = 0;
		}
		if (!recv_all(r->sock, r->packet->data + cfg, size, &r->stop)) {
			av_packet_unref(r->packet);
			break;
		}

		r->packet->pts = (int64_t)(pts_flags & SC_PACKET_PTS_MASK);
		r->packet->dts = r->packet->pts;
		if (is_key)
			r->packet->flags |= AV_PKT_FLAG_KEY;

		int send_ret = avcodec_send_packet(r->codec_ctx, r->packet);
		av_packet_unref(r->packet);
		if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
			char errbuf[128];
			av_strerror(send_ret, errbuf, sizeof(errbuf));
			obs_log(LOG_WARNING, "scrcpy-reader: send_packet: %s", errbuf);
			continue;
		}

		for (;;) {
			int rcv = avcodec_receive_frame(r->codec_ctx, r->frame);
			if (rcv == AVERROR(EAGAIN) || rcv == AVERROR_EOF)
				break;
			if (rcv < 0) {
				char errbuf[128];
				av_strerror(rcv, errbuf, sizeof(errbuf));
				obs_log(LOG_WARNING, "scrcpy-reader: receive_frame: %s", errbuf);
				break;
			}
			emit_frame(r, r->frame);
			av_frame_unref(r->frame);
		}
	}

done:
	os_atomic_set_bool(&r->running, false);
	obs_log(LOG_INFO, "scrcpy-reader: thread exiting");
	return NULL;
}

scrcpy_reader_t *scrcpy_reader_create(obs_source_t *source, uint16_t port, bool hardware_decoding, bool flip_vertical,
				      int video_buffer_ms, bool portrait_mode, int color_range_override)
{
	struct scrcpy_reader *r = bzalloc(sizeof(*r));
	r->source = source;
	r->port = port;
	r->sock = INVALID_SOCK;
	r->hardware_decoding = hardware_decoding;
	r->flip_vertical = flip_vertical;
	r->video_buffer_ms = video_buffer_ms > 0 ? video_buffer_ms : 0;
	r->portrait_mode = portrait_mode;
	r->color_range_override = color_range_override;
	r->hw_pix_fmt = AV_PIX_FMT_NONE;
	r->stop = false;
	r->running = false;
	r->last_frame_ns = 0;
	pthread_mutex_init(&r->state_mutex, NULL);

	if (pthread_create(&r->thread, NULL, reader_thread, r) != 0) {
		obs_log(LOG_ERROR, "scrcpy-reader: pthread_create failed");
		pthread_mutex_destroy(&r->state_mutex);
		bfree(r);
		return NULL;
	}
	r->thread_started = true;
	return r;
}

void scrcpy_reader_set_color_range(scrcpy_reader_t *r, int color_range_override)
{
	if (!r)
		return;

	if (color_range_override != SCRCPY_COLOR_RANGE_AUTO &&
	    color_range_override != SCRCPY_COLOR_RANGE_FULL &&
	    color_range_override != SCRCPY_COLOR_RANGE_LIMITED)
		color_range_override = SCRCPY_COLOR_RANGE_AUTO;

	pthread_mutex_lock(&r->state_mutex);
	r->color_range_override = color_range_override;
	pthread_mutex_unlock(&r->state_mutex);
}

bool scrcpy_reader_is_alive(const scrcpy_reader_t *r)
{
	return r && os_atomic_load_bool(&r->running);
}

bool scrcpy_reader_is_stale(const scrcpy_reader_t *r, uint32_t max_age_ms)
{
	if (!r || !os_atomic_load_bool(&r->running))
		return true;

	pthread_mutex_lock((pthread_mutex_t *)&r->state_mutex);
	uint64_t last_frame_ns = r->last_frame_ns;
	pthread_mutex_unlock((pthread_mutex_t *)&r->state_mutex);

	if (last_frame_ns == 0)
		return false;

	uint64_t now = os_gettime_ns();
	uint64_t max_age_ns = (uint64_t)max_age_ms * UINT64_C(1000000);
	return now > last_frame_ns && now - last_frame_ns > max_age_ns;
}

void scrcpy_reader_destroy(scrcpy_reader_t *r)
{
	if (!r)
		return;

	os_atomic_set_bool(&r->stop, true);

	/* Shutting down the socket unblocks any in-flight recv(). */
	if (r->sock != INVALID_SOCK) {
#ifdef _WIN32
		shutdown(r->sock, SD_BOTH);
#else
		shutdown(r->sock, SHUT_RDWR);
#endif
	}

	if (r->thread_started)
		pthread_join(r->thread, NULL);

	if (r->sock != INVALID_SOCK) {
		close_sock(r->sock);
		r->sock = INVALID_SOCK;
	}
	if (r->packet)
		av_packet_free(&r->packet);
	if (r->frame)
		av_frame_free(&r->frame);
	if (r->transfer_frame)
		av_frame_free(&r->transfer_frame);
	if (r->portrait_frame)
		av_frame_free(&r->portrait_frame);
	if (r->hw_device_ctx)
		av_buffer_unref(&r->hw_device_ctx);
	if (r->codec_ctx)
		avcodec_free_context(&r->codec_ctx);
	if (r->pending_config)
		bfree(r->pending_config);

	pthread_mutex_destroy(&r->state_mutex);
	bfree(r);
}
