#include "scrcpy-color-transform.h"

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>

#include <stdio.h>
#include <string.h>

struct scrcpy_color_transform {
	int source_profile;
	int target_profile;
	AVFilterGraph *graph;
	AVFilterContext *buffer_src;
	AVFilterContext *buffer_sink;
	enum AVPixelFormat input_format;
	int input_width;
	int input_height;
};

struct cst_target_info {
	const char *primaries;
	const char *matrix;
	const char *trc;
	enum AVColorPrimaries av_primaries;
	enum AVColorSpace av_space;
	enum AVColorTransferCharacteristic av_trc;
	bool hdr;
	enum AVPixelFormat output_format;
};

static bool profile_to_colorimetry(int profile, const char **primaries, const char **matrix, const char **trc,
				   enum AVColorPrimaries *av_primaries, enum AVColorSpace *av_space,
				   enum AVColorTransferCharacteristic *av_trc)
{
	const int color_space = profile / 10;
	const int gamma = profile % 10;

	if (profile <= 0 || color_space < 1 || color_space > 3 || gamma < 1 || gamma > 9)
		return false;

	if (color_space == 3) {
		*primaries = "bt2020";
		*matrix = "2020_ncl";
		*av_primaries = AVCOL_PRI_BT2020;
		*av_space = AVCOL_SPC_BT2020_NCL;
	} else {
		*primaries = "bt709";
		*matrix = "709";
		*av_primaries = AVCOL_PRI_BT709;
		*av_space = AVCOL_SPC_BT709;
	}

	switch (gamma) {
	case 1:
		*trc = "gamma22";
		*av_trc = AVCOL_TRC_GAMMA22;
		break;
	case 2:
		/*
		 * FFmpeg does not expose a dedicated exact gamma-2.4 transfer in
		 * colorspace/zscale; SMPTE 170M is the standard SDR transfer used
		 * by the plugin's current Gamma 2.4 camera path.
		 */
		*trc = "smpte170m";
		*av_trc = AVCOL_TRC_SMPTE170M;
		break;
	case 3:
		*trc = "bt709";
		*av_trc = AVCOL_TRC_BT709;
		break;
	case 4:
	case 8:
		*trc = "srgb";
		*av_trc = AVCOL_TRC_IEC61966_2_1;
		break;
	case 5:
		*trc = "arib-std-b67";
		*av_trc = AVCOL_TRC_ARIB_STD_B67;
		break;
	case 6:
	case 7:
		*trc = "smpte2084";
		*av_trc = AVCOL_TRC_SMPTE2084;
		break;
	case 9:
		*trc = "linear";
		*av_trc = AVCOL_TRC_LINEAR;
		break;
	default:
		return false;
	}

	return true;
}

static bool target_info(int profile, struct cst_target_info *info)
{
	const char *primaries;
	const char *matrix;
	const char *trc;
	enum AVColorPrimaries av_primaries;
	enum AVColorSpace av_space;
	enum AVColorTransferCharacteristic av_trc;

	if (!info ||
	    !profile_to_colorimetry(profile, &primaries, &matrix, &trc, &av_primaries, &av_space, &av_trc))
		return false;

	info->primaries = primaries;
	info->matrix = matrix;
	info->trc = trc;
	info->av_primaries = av_primaries;
	info->av_space = av_space;
	info->av_trc = av_trc;
	info->hdr = (profile % 10) >= 5 && (profile % 10) <= 7;
	info->output_format = info->hdr ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
	return true;
}

static bool source_is_hdr(const scrcpy_color_transform_t *transform, const AVFrame *input)
{
	if (transform->source_profile > 0) {
		const int gamma = transform->source_profile % 10;
		return gamma == 5 || gamma == 6 || gamma == 7;
	}

	return input && (input->color_trc == AVCOL_TRC_ARIB_STD_B67 || input->color_trc == AVCOL_TRC_SMPTE2084 ||
			 input->color_trc == AVCOL_TRC_BT2020_10 || input->color_trc == AVCOL_TRC_BT2020_12);
}

static void frame_colorimetry(const AVFrame *input, const char **primaries, const char **matrix, const char **trc)
{
	*primaries = input->color_primaries == AVCOL_PRI_BT2020 ? "bt2020" : "bt709";

	switch (input->colorspace) {
	case AVCOL_SPC_BT2020_CL:
		*matrix = "2020_cl";
		break;
	case AVCOL_SPC_BT2020_NCL:
		*matrix = "2020_ncl";
		break;
	case AVCOL_SPC_BT709:
		*matrix = "709";
		break;
	default:
		*matrix = input->color_primaries == AVCOL_PRI_BT2020 ? "2020_ncl" : "709";
		break;
	}

	switch (input->color_trc) {
	case AVCOL_TRC_BT709:
		*trc = "bt709";
		break;
	case AVCOL_TRC_GAMMA22:
		*trc = "gamma22";
		break;
	case AVCOL_TRC_SMPTE170M:
		*trc = "smpte170m";
		break;
	case AVCOL_TRC_IEC61966_2_1:
		*trc = "srgb";
		break;
	case AVCOL_TRC_LINEAR:
		*trc = "linear";
		break;
	case AVCOL_TRC_ARIB_STD_B67:
		*trc = "arib-std-b67";
		break;
	case AVCOL_TRC_SMPTE2084:
		*trc = "smpte2084";
		break;
	case AVCOL_TRC_BT2020_10:
		*trc = "bt2020-10";
		break;
	case AVCOL_TRC_BT2020_12:
		*trc = "bt2020-12";
		break;
	default:
		*trc = "srgb";
		break;
	}
}

static void source_colorimetry(const scrcpy_color_transform_t *transform, const AVFrame *input,
			       const char **primaries, const char **matrix, const char **trc)
{
	enum AVColorPrimaries av_primaries;
	enum AVColorSpace av_space;
	enum AVColorTransferCharacteristic av_trc;

	if (transform->source_profile > 0 &&
	    profile_to_colorimetry(transform->source_profile, primaries, matrix, trc, &av_primaries, &av_space, &av_trc))
		return;

	frame_colorimetry(input, primaries, matrix, trc);
}

static bool create_filter(AVFilterGraph *graph, const char *name, const char *instance, const char *args,
			  AVFilterContext **out)
{
	const AVFilter *filter = avfilter_get_by_name(name);
	int ret;

	if (!filter)
		return false;

	ret = avfilter_graph_create_filter(out, filter, instance, args, NULL, graph);
	return ret >= 0;
}

static void log_filter_error(const char *stage, int error)
{
	char error_string[AV_ERROR_MAX_STRING_SIZE];
	av_strerror(error, error_string, sizeof(error_string));
	av_log(NULL, AV_LOG_WARNING, "scrcpy-color-transform: %s failed: %s\n", stage, error_string);
}

static enum AVPixelFormat normalize_input_format(enum AVPixelFormat format)
{
	switch (format) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUV444P:
	case AV_PIX_FMT_YUV420P10LE:
	case AV_PIX_FMT_YUV422P10LE:
	case AV_PIX_FMT_YUV444P10LE:
		return format;
	case AV_PIX_FMT_P010LE:
		return AV_PIX_FMT_YUV420P10LE;
	case AV_PIX_FMT_NV12:
	case AV_PIX_FMT_YUVJ420P:
		return AV_PIX_FMT_YUV420P;
	default:
		return AV_PIX_FMT_YUV420P;
	}
}

static bool build_graph(scrcpy_color_transform_t *transform, const AVFrame *input)
{
	struct cst_target_info target;
	AVFilterContext *current = NULL;
	AVFilterContext *normalizer = NULL;
	AVFilterContext *first = NULL;
	AVFilterContext *second = NULL;
	AVFilterContext *third = NULL;
	AVFilterContext *sink = NULL;
	char buffer_args[256];
	char args[1024];
	const enum AVPixelFormat input_format = normalize_input_format((enum AVPixelFormat)input->format);
	const bool normalize = input_format != (enum AVPixelFormat)input->format;
	int ret;

	if (!target_info(transform->target_profile, &target))
		return false;

	avfilter_graph_free(&transform->graph);
	transform->buffer_src = NULL;
	transform->buffer_sink = NULL;

	transform->graph = avfilter_graph_alloc();
	if (!transform->graph)
		return false;

	const char *pix_fmt_name = av_get_pix_fmt_name(input_format);
	if (!pix_fmt_name)
		goto fail;

	snprintf(buffer_args, sizeof(buffer_args), "video_size=%dx%d:pix_fmt=%s:time_base=1/1000000:pixel_aspect=1/1",
		 input->width, input->height, pix_fmt_name);

	if (!create_filter(transform->graph, "buffer", "in", buffer_args, &transform->buffer_src))
		goto fail;
	current = transform->buffer_src;

	if (normalize) {
		snprintf(args, sizeof(args), "pix_fmts=%s", pix_fmt_name);
		if (!create_filter(transform->graph, "format", "normalize", args, &normalizer))
			goto fail;
		ret = avfilter_link(current, 0, normalizer, 0);
		if (ret < 0) {
			log_filter_error("link normalize", ret);
			goto fail;
		}
		current = normalizer;
	}

	const char *source_primaries;
	const char *source_matrix;
	const char *source_trc;
	source_colorimetry(transform, input, &source_primaries, &source_matrix, &source_trc);

	if (source_is_hdr(transform, input) && !target.hdr) {
		/*
		 * HDR -> SDR requires a real tone-map in linear light. A plain
		 * colorspace conversion would clip highlights above the SDR range.
		 */
		snprintf(args, sizeof(args),
			 "primariesin=%s:matrixin=%s:transferin=%s:primaries=%s:matrix=gbr:transfer=linear:range=pc",
			 source_primaries, source_matrix, source_trc, source_primaries);
		if (!create_filter(transform->graph, "zscale", "hdr-linearize", args, &first))
			goto fail;
		ret = avfilter_link(current, 0, first, 0);
		if (ret < 0) {
			log_filter_error("link HDR linearize", ret);
			goto fail;
		}
		current = first;

		if (!create_filter(transform->graph, "format", "float-rgb", "pix_fmts=gbrpf32le", &second))
			goto fail;
		ret = avfilter_link(current, 0, second, 0);
		if (ret < 0) {
			log_filter_error("link float RGB", ret);
			goto fail;
		}
		current = second;

		if (!create_filter(transform->graph, "tonemap", "tone-map", "tonemap=mobius:param=0.3:desat=2", &third))
			goto fail;
		ret = avfilter_link(current, 0, third, 0);
		if (ret < 0) {
			log_filter_error("link tone map", ret);
			goto fail;
		}
		current = third;

		snprintf(args, sizeof(args),
			 "matrixin=gbr:transferin=linear:primariesin=%s:matrix=%s:transfer=linear:primaries=%s:rangein=pc:range=tv",
			 source_primaries, target.matrix, target.primaries);
		if (!create_filter(transform->graph, "zscale", "linear-target", args, &first))
			goto fail;
		ret = avfilter_link(current, 0, first, 0);
		if (ret < 0) {
			log_filter_error("link linear target", ret);
			goto fail;
		}
		current = first;

		if (!create_filter(transform->graph, "format", "linear-yuv10", "pix_fmts=yuv420p10le", &second))
			goto fail;
		ret = avfilter_link(current, 0, second, 0);
		if (ret < 0) {
			log_filter_error("link linear YUV", ret);
			goto fail;
		}
		current = second;

		snprintf(args, sizeof(args),
			 "iprimaries=%s:ispace=%s:itrc=linear:irange=tv:primaries=%s:space=%s:trc=%s:range=tv:format=yuv420p:dither=fsb",
			 target.primaries, target.matrix, target.primaries, target.matrix, target.trc);
		if (!create_filter(transform->graph, "colorspace", "target-transfer", args, &first))
			goto fail;
		ret = avfilter_link(current, 0, first, 0);
		if (ret < 0) {
			log_filter_error("link target transfer", ret);
			goto fail;
		}
		current = first;
	} else if (target.hdr) {
		/*
		 * SDR -> HDR and HDR -> HDR are handled by zscale because it can
		 * represent HLG/PQ transfers as well as the 10-bit output format.
		 */
		snprintf(args, sizeof(args),
			 "primariesin=%s:matrixin=%s:transferin=%s:primaries=%s:matrix=%s:transfer=%s:range=tv",
			 source_primaries, source_matrix, source_trc, target.primaries, target.matrix, target.trc);
		if (!create_filter(transform->graph, "zscale", "target-hdr", args, &first))
			goto fail;
		ret = avfilter_link(current, 0, first, 0);
		if (ret < 0) {
			log_filter_error("link target HDR", ret);
			goto fail;
		}
		current = first;

		if (!create_filter(transform->graph, "format", "target-10bit", "pix_fmts=yuv420p10le", &second))
			goto fail;
		ret = avfilter_link(current, 0, second, 0);
		if (ret < 0) {
			log_filter_error("link target 10-bit", ret);
			goto fail;
		}
		current = second;
	} else {
		/*
		 * SDR -> SDR is handled by colorspace with fast=0 so primary and
		 * transfer conversion is done mathematically rather than by
		 * metadata substitution.
		 */
		snprintf(args, sizeof(args),
			 "iprimaries=%s:ispace=%s:itrc=%s:irange=input:primaries=%s:space=%s:trc=%s:range=tv:format=yuv420p:dither=fsb:fast=0:wpadapt=bradford",
			 source_primaries, source_matrix, source_trc, target.primaries, target.matrix, target.trc);
		if (!create_filter(transform->graph, "colorspace", "target-sdr", args, &first))
			goto fail;
		ret = avfilter_link(current, 0, first, 0);
		if (ret < 0) {
			log_filter_error("link target SDR", ret);
			goto fail;
		}
		current = first;
	}

	if (!create_filter(transform->graph, "buffersink", "out", NULL, &sink))
		goto fail;

	ret = avfilter_link(current, 0, sink, 0);
	if (ret < 0) {
		log_filter_error("link sink", ret);
		goto fail;
	}

	ret = avfilter_graph_config(transform->graph, NULL);
	if (ret < 0) {
		log_filter_error("configure graph", ret);
		goto fail;
	}

	transform->buffer_sink = sink;
	transform->input_format = (enum AVPixelFormat)input->format;
	transform->input_width = input->width;
	transform->input_height = input->height;
	return true;

fail:
	avfilter_graph_free(&transform->graph);
	transform->buffer_src = NULL;
	transform->buffer_sink = NULL;
	return false;
}

scrcpy_color_transform_t *scrcpy_color_transform_create(int source_profile, int target_profile)
{
	scrcpy_color_transform_t *transform;

	if (target_profile == SCRCPY_CST_OFF)
		return NULL;

	transform = av_mallocz(sizeof(*transform));
	if (!transform)
		return NULL;

	transform->source_profile = source_profile;
	transform->target_profile = target_profile;
	transform->input_format = AV_PIX_FMT_NONE;
	return transform;
}

bool scrcpy_color_transform_apply(scrcpy_color_transform_t *transform, AVFrame *input, AVFrame *output)
{
	struct cst_target_info target;
	int ret;

	if (!transform || !input || !output || !target_info(transform->target_profile, &target))
		return false;

	if (!transform->graph || transform->input_format != (enum AVPixelFormat)input->format ||
	    transform->input_width != input->width || transform->input_height != input->height) {
		if (!build_graph(transform, input))
			return false;
	}

	ret = av_buffersrc_add_frame_flags(transform->buffer_src, input, AV_BUFFERSRC_FLAG_KEEP_REF);
	if (ret < 0) {
		log_filter_error("push frame", ret);
		return false;
	}

	av_frame_unref(output);
	ret = av_buffersink_get_frame(transform->buffer_sink, output);
	if (ret < 0) {
		log_filter_error("pull frame", ret);
		return false;
	}

	output->color_primaries = target.av_primaries;
	output->colorspace = target.av_space;
	output->color_trc = target.av_trc;
	output->color_range = AVCOL_RANGE_MPEG;
	return true;
}

bool scrcpy_color_transform_target_is_hdr(int target_profile)
{
	struct cst_target_info target;
	return target_info(target_profile, &target) && target.hdr;
}

void scrcpy_color_transform_destroy(scrcpy_color_transform_t *transform)
{
	if (!transform)
		return;

	avfilter_graph_free(&transform->graph);
	av_free(transform);
}
