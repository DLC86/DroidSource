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
#include <libswscale/swscale.h>

#include <stdint.h>
#include <math.h>
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
	char color_space[16];
	char color_range[16];
	char transfer[16];

	AVBufferRef *hw_device_ctx;
	enum AVPixelFormat hw_pix_fmt;
	AVFrame *transfer_frame;
	struct SwsContext *convert_ctx;
	AVFrame *converted_frame;
	AVFrame *rgb_frame;
	struct SwsContext *to_rgb_ctx;
	struct SwsContext *from_rgb_ctx;
	enum AVPixelFormat requested_pix_fmt;

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

static enum AVPixelFormat obs_to_av_pixfmt(const char *format)
{
	if (!format || strcmp(format, "auto") == 0)
		return AV_PIX_FMT_NONE;
	if (strcmp(format, "i420") == 0)
		return AV_PIX_FMT_YUV420P;
	if (strcmp(format, "nv12") == 0)
		return AV_PIX_FMT_NV12;
	return AV_PIX_FMT_NONE;
}

static enum video_colorspace parse_color_space(const char *value)
{
	if (!value || strcmp(value, "auto") == 0)
		return VIDEO_CS_DEFAULT;
	if (strcmp(value, "601") == 0)
		return VIDEO_CS_601;
	if (strcmp(value, "709") == 0)
		return VIDEO_CS_709;
	if (strcmp(value, "srgb") == 0)
		return VIDEO_CS_SRGB;
	if (strcmp(value, "2100pq") == 0)
		return VIDEO_CS_2100_PQ;
	if (strcmp(value, "2100hlg") == 0)
		return VIDEO_CS_2100_HLG;
	return VIDEO_CS_DEFAULT;
}

static enum video_range_type parse_color_range(const char *value)
{
	if (!value || strcmp(value, "auto") == 0)
		return VIDEO_RANGE_DEFAULT;
	if (strcmp(value, "full") == 0)
		return VIDEO_RANGE_FULL;
	if (strcmp(value, "limited") == 0)
		return VIDEO_RANGE_PARTIAL;
	return VIDEO_RANGE_DEFAULT;
}

static enum video_trc parse_transfer(const char *value, enum video_colorspace cs)
{
	if (!value || strcmp(value, "auto") == 0) {
		if (cs == VIDEO_CS_2100_PQ)
			return VIDEO_TRC_PQ;
		if (cs == VIDEO_CS_2100_HLG)
			return VIDEO_TRC_HLG;
		return VIDEO_TRC_SRGB;
	}
	if (strcmp(value, "srgb") == 0)
		return VIDEO_TRC_SRGB;
	if (strcmp(value, "hlg") == 0)
		return VIDEO_TRC_HLG;
	if (strcmp(value, "pq") == 0)
		return VIDEO_TRC_PQ;
	return VIDEO_TRC_SRGB;
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
			if (av_hwdevice_ctx_create(&r->hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA,
						NULL, NULL, 0) == 0) {
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
	r->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

	if (avcodec_open2(r->codec_ctx, codec, NULL) < 0) {
		obs_log(LOG_ERROR, "scrcpy-reader: avcodec_open2 failed");
		avcodec_free_context(&r->codec_ctx);
		return false;
	}

	r->packet = av_packet_alloc();
	r->frame = av_frame_alloc();
	r->transfer_frame = av_frame_alloc();
	r->converted_frame = av_frame_alloc();
	r->rgb_frame = av_frame_alloc();
	if (!r->packet || !r->frame || !r->transfer_frame || !r->converted_frame || !r->rgb_frame) {
		obs_log(LOG_ERROR, "scrcpy-reader: av alloc failed");
		return false;
	}
	return true;
}

static enum AVColorTransferCharacteristic source_trc_from_frame(const AVFrame *f)
{
	if (!f)
		return AVCOL_TRC_UNSPECIFIED;
	return (enum AVColorTransferCharacteristic)f->color_trc;
}

enum color_xfer {
	XFER_SRGB,
	XFER_HLG,
	XFER_PQ,
};

static enum color_xfer parse_target_xfer(const char *value)
{
	if (value && strcmp(value, "hlg") == 0)
		return XFER_HLG;
	if (value && strcmp(value, "pq") == 0)
		return XFER_PQ;
	return XFER_SRGB;
}

static enum color_xfer source_xfer_from_frame(const AVFrame *f)
{
	switch (source_trc_from_frame(f)) {
	case AVCOL_TRC_ARIB_STD_B67:
		return XFER_HLG;
	case AVCOL_TRC_SMPTE2084:
		return XFER_PQ;
	default:
		return XFER_SRGB;
	}
}

static int sws_cs_from_av(const AVFrame *f)
{
	switch ((enum AVColorSpace)f->colorspace) {
	case AVCOL_SPC_BT709:
		return SWS_CS_ITU709;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
		return SWS_CS_ITU601;
	case AVCOL_SPC_SMPTE240M:
		return SWS_CS_SMPTE240M;
	case AVCOL_SPC_BT2020_NCL:
	case AVCOL_SPC_BT2020_CL:
		return SWS_CS_BT2020;
	default:
		return SWS_CS_ITU709;
	}
}

static int sws_cs_from_output(const char *value, int source_cs)
{
	if (!value || strcmp(value, "auto") == 0)
		return source_cs;
	if (strcmp(value, "601") == 0)
		return SWS_CS_ITU601;
	if (strcmp(value, "709") == 0 || strcmp(value, "srgb") == 0)
		return SWS_CS_ITU709;
	if (strcmp(value, "2100pq") == 0 || strcmp(value, "2100hlg") == 0)
		return SWS_CS_BT2020;
	return source_cs;
}

static enum video_colorspace obs_cs_from_output(const char *value, int source_cs)
{
	if (!value || strcmp(value, "auto") == 0) {
		switch (source_cs) {
		case SWS_CS_ITU601:
			return VIDEO_CS_601;
		case SWS_CS_BT2020:
			return VIDEO_CS_2100_PQ;
		default:
			return VIDEO_CS_709;
		}
	}
	if (strcmp(value, "601") == 0)
		return VIDEO_CS_601;
	if (strcmp(value, "709") == 0)
		return VIDEO_CS_709;
	if (strcmp(value, "srgb") == 0)
		return VIDEO_CS_SRGB;
	if (strcmp(value, "2100pq") == 0)
		return VIDEO_CS_2100_PQ;
	if (strcmp(value, "2100hlg") == 0)
		return VIDEO_CS_2100_HLG;
	return VIDEO_CS_DEFAULT;
}

static bool output_is_full_range(const char *value, const AVFrame *f)
{
	if (value && strcmp(value, "full") == 0)
		return true;
	if (value && strcmp(value, "limited") == 0)
		return false;
	return f->color_range == AVCOL_RANGE_JPEG;
}

static double decode_xfer(double v, enum color_xfer xfer)
{
	if (xfer == XFER_HLG) {
		if (v <= 0.5)
			return (v * v) / 3.0;
		const double a = 0.17883277;
		const double b = 0.28466892;
		const double c = 0.55991073;
		return (exp((v - c) / a) + b) / 12.0;
	}
	if (xfer == XFER_PQ) {
		/* Treat SDR white as 100 nits when moving into PQ. */
		const double m1 = 2610.0 / 16384.0;
		const double m2 = 2523.0 / 32.0;
		const double c1 = 3424.0 / 4096.0;
		const double c2 = 2413.0 / 128.0;
		const double c3 = 2392.0 / 128.0;
		const double p = pow(fmax(v, 0.0), 1.0 / m2);
		double n = pow(fmax(p - c1, 0.0) / fmax(c2 - c3 * p, 1e-9), 1.0 / m1);
		return n * 10000.0 / 100.0;
	}
	if (v <= 0.04045)
		return v / 12.92;
	return pow((v + 0.055) / 1.055, 2.4);
}

static double encode_xfer(double v, enum color_xfer xfer)
{
	v = fmax(0.0, fmin(1.0, v));
	if (xfer == XFER_HLG) {
		const double a = 0.17883277;
		const double b = 0.28466892;
		const double c = 0.55991073;
		if (v <= 1.0 / 12.0)
			return sqrt(3.0 * v);
		return a * log(12.0 * v - b) + c;
	}
	if (xfer == XFER_PQ) {
		const double m1 = 2610.0 / 16384.0;
		const double m2 = 2523.0 / 32.0;
		const double c1 = 3424.0 / 4096.0;
		const double c2 = 2413.0 / 128.0;
		const double c3 = 2392.0 / 128.0;
		const double nits = v * 100.0;
		const double l = fmax(nits / 10000.0, 0.0);
		const double p = pow(l, m1);
		const double n = pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
		return n;
	}
	if (v <= 0.0031308)
		return 12.92 * v;
	return 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

static void build_xfer_lut(uint8_t lut[256], enum color_xfer src, enum color_xfer dst)
{
	for (int i = 0; i < 256; ++i) {
		double code = i / 255.0;
		double linear = decode_xfer(code, src);
		double encoded = encode_xfer(linear, dst);
		int out = (int)lrint(fmax(0.0, fmin(1.0, encoded)) * 255.0);
		lut[i] = (uint8_t)out;
	}
}

static void apply_xfer_lut(AVFrame *frame, const uint8_t lut[256])
{
	if (!frame || frame->format != AV_PIX_FMT_RGB24)
		return;
	for (int y = 0; y < frame->height; ++y) {
		uint8_t *row = frame->data[0] + (size_t)y * frame->linesize[0];
		for (int x = 0; x < frame->width; ++x) {
			uint8_t *p = row + x * 3;
			p[0] = lut[p[0]];
			p[1] = lut[p[1]];
			p[2] = lut[p[2]];
		}
	}
}

static void emit_frame(struct scrcpy_reader *r, AVFrame *f)
{
	AVFrame *out = f;

	if (r->hw_pix_fmt != AV_PIX_FMT_NONE && f->format == r->hw_pix_fmt) {
		av_frame_unref(r->transfer_frame);
		if (av_hwframe_transfer_data(r->transfer_frame, f, 0) < 0) {
			obs_log(LOG_WARNING, "scrcpy-reader: hardware frame transfer failed");
			return;
		}
		out = r->transfer_frame;
	}

	bool want_processing =
		(r->color_space && strcmp(r->color_space, "auto") != 0) ||
		(r->color_range && strcmp(r->color_range, "auto") != 0) ||
		(r->transfer && strcmp(r->transfer, "auto") != 0);

	enum AVPixelFormat target_fmt = r->requested_pix_fmt != AV_PIX_FMT_NONE
		? r->requested_pix_fmt
		: (enum AVPixelFormat)out->format;

	AVColorRange src_range = out->color_range;
	bool target_full = output_is_full_range(r->color_range, out);
	int src_cs = sws_cs_from_av(out);
	int dst_cs = sws_cs_from_output(r->color_space, src_cs);
	enum color_xfer src_xfer = source_xfer_from_frame(out);
	enum color_xfer dst_xfer = parse_target_xfer(r->transfer);
	bool range_changed = r->color_range && strcmp(r->color_range, "auto") != 0 &&
		((target_full && src_range != AVCOL_RANGE_JPEG) || (!target_full && src_range == AVCOL_RANGE_JPEG));
	bool cs_changed = r->color_space && strcmp(r->color_space, "auto") != 0 && dst_cs != src_cs;
	bool xfer_changed = r->transfer && strcmp(r->transfer, "auto") != 0 && dst_xfer != src_xfer;

	if (want_processing && (range_changed || cs_changed || xfer_changed)) {
		r->to_rgb_ctx = sws_getCachedContext(r->to_rgb_ctx,
			(int)out->width, (int)out->height, (enum AVPixelFormat)out->format,
			(int)out->width, (int)out->height, AV_PIX_FMT_RGB24,
			SWS_FAST_BILINEAR, NULL, NULL, NULL);
		if (!r->to_rgb_ctx) {
			obs_log(LOG_WARNING, "scrcpy-reader: RGB processing context creation failed");
			return;
		}
		sws_setColorspaceDetails(r->to_rgb_ctx, sws_getCoefficients(src_cs),
			src_range == AVCOL_RANGE_JPEG, sws_getCoefficients(dst_cs), 1, 0, 1 << 16, 1 << 16);

		av_frame_unref(r->rgb_frame);
		r->rgb_frame->format = AV_PIX_FMT_RGB24;
		r->rgb_frame->width = out->width;
		r->rgb_frame->height = out->height;
		if (av_frame_get_buffer(r->rgb_frame, 32) < 0)
			return;
		sws_scale(r->to_rgb_ctx, (const uint8_t * const *)out->data, out->linesize,
			0, (int)out->height, r->rgb_frame->data, r->rgb_frame->linesize);

		if (xfer_changed) {
			uint8_t lut[256];
			build_xfer_lut(lut, src_xfer, dst_xfer);
			apply_xfer_lut(r->rgb_frame, lut);
		}

		r->from_rgb_ctx = sws_getCachedContext(r->from_rgb_ctx,
			(int)r->rgb_frame->width, (int)r->rgb_frame->height, AV_PIX_FMT_RGB24,
			(int)r->rgb_frame->width, (int)r->rgb_frame->height, target_fmt,
			SWS_FAST_BILINEAR, NULL, NULL, NULL);
		if (!r->from_rgb_ctx)
			return;
		sws_setColorspaceDetails(r->from_rgb_ctx, sws_getCoefficients(dst_cs), 1,
			sws_getCoefficients(dst_cs), target_full ? 1 : 0, 0, 1 << 16, 1 << 16);

		av_frame_unref(r->converted_frame);
		r->converted_frame->format = target_fmt;
		r->converted_frame->width = out->width;
		r->converted_frame->height = out->height;
		if (av_frame_get_buffer(r->converted_frame, 32) < 0)
			return;
		sws_scale(r->from_rgb_ctx, (const uint8_t * const *)r->rgb_frame->data,
			r->rgb_frame->linesize, 0, r->rgb_frame->height,
			r->converted_frame->data, r->converted_frame->linesize);
		out = r->converted_frame;
	} else if (r->requested_pix_fmt != AV_PIX_FMT_NONE && out->format != r->requested_pix_fmt) {
		r->from_rgb_ctx = sws_getCachedContext(r->from_rgb_ctx,
			(int)out->width, (int)out->height, (enum AVPixelFormat)out->format,
			(int)out->width, (int)out->height, r->requested_pix_fmt,
			SWS_FAST_BILINEAR, NULL, NULL, NULL);
		if (!r->from_rgb_ctx)
			return;
		av_frame_unref(r->converted_frame);
		r->converted_frame->format = r->requested_pix_fmt;
		r->converted_frame->width = out->width;
		r->converted_frame->height = out->height;
		if (av_frame_get_buffer(r->converted_frame, 32) < 0)
			return;
		sws_scale(r->from_rgb_ctx, (const uint8_t * const *)out->data, out->linesize,
			0, (int)out->height, r->converted_frame->data, r->converted_frame->linesize);
		out = r->converted_frame;
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
	pthread_mutex_unlock(&r->state_mutex);

	enum video_colorspace cs = obs_cs_from_output(r->color_space, dst_cs);
	enum video_range_type range = target_full ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
	video_format_get_parameters_for_format(cs, range, fmt, obs_frame.color_matrix, obs_frame.color_range_min,
						obs_frame.color_range_max);
	obs_frame.full_range = range == VIDEO_RANGE_FULL;
	obs_frame.trc = (uint8_t)parse_transfer(r->transfer, cs);

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

scrcpy_reader_t *scrcpy_reader_create(obs_source_t *source, uint16_t port, bool hardware_decoding,
							bool flip_vertical, int video_buffer_ms, const char *pixel_format,
							const char *color_space, const char *color_range, const char *transfer)
{
	struct scrcpy_reader *r = bzalloc(sizeof(*r));
	r->source = source;
	r->port = port;
	r->sock = INVALID_SOCK;
	r->hardware_decoding = hardware_decoding;
	r->flip_vertical = flip_vertical;
	r->video_buffer_ms = video_buffer_ms > 0 ? video_buffer_ms : 0;
	r->hw_pix_fmt = AV_PIX_FMT_NONE;
	r->requested_pix_fmt = obs_to_av_pixfmt(pixel_format);
	snprintf(r->color_space, sizeof(r->color_space), "%s", color_space && *color_space ? color_space : "auto");
	snprintf(r->color_range, sizeof(r->color_range), "%s", color_range && *color_range ? color_range : "auto");
	snprintf(r->transfer, sizeof(r->transfer), "%s", transfer && *transfer ? transfer : "auto");
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
	if (r->converted_frame)
		av_frame_free(&r->converted_frame);
	if (r->rgb_frame)
		av_frame_free(&r->rgb_frame);
	if (r->to_rgb_ctx)
		sws_freeContext(r->to_rgb_ctx);
	if (r->from_rgb_ctx)
		sws_freeContext(r->from_rgb_ctx);
	if (r->convert_ctx)
		sws_freeContext(r->convert_ctx);
	if (r->hw_device_ctx)
		av_buffer_unref(&r->hw_device_ctx);
	if (r->codec_ctx)
		avcodec_free_context(&r->codec_ctx);
	if (r->pending_config)
		bfree(r->pending_config);

	pthread_mutex_destroy(&r->state_mutex);
	bfree(r);
}
