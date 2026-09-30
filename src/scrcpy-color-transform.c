#include "scrcpy-color-transform.h"

#include <obs-module.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>

#include <stdio.h>

struct scrcpy_color_transform {
	int source_profile;
	int target_profile;
	AVFilterGraph *graph;
	AVFilterContext *buffer_src;
	AVFilterContext *buffer_sink;
	enum AVPixelFormat input_format;
	enum AVColorRange input_range;
	int input_width;
	int input_height;
};

struct cst_color_info {
	const char *primaries;
	const char *matrix;
	const char *trc;
	enum AVColorPrimaries av_primaries;
	enum AVColorSpace av_space;
	enum AVColorTransferCharacteristic av_trc;
	bool hdr;
	enum AVPixelFormat output_format;
};

static bool profile_to_color_info(int profile, struct cst_color_info *info)
{
	const int color_space = profile / 10;
	const int gamma = profile % 10;

	if (!info || profile <= 0 || color_space < 1 || color_space > 3 || gamma < 1 || gamma > 9)
		return false;

	info->primaries = color_space == 3 ? "bt2020" : "bt709";
	info->matrix = color_space == 3 ? "2020_ncl" : "709";
	info->av_primaries = color_space == 3 ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
	info->av_space = color_space == 3 ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;

	switch (gamma) {
	case 1:
		info->trc = "bt470m";
		info->av_trc = AVCOL_TRC_GAMMA22;
		break;
	case 2:
		info->trc = "smpte170m";
		info->av_trc = AVCOL_TRC_SMPTE170M;
		break;
	case 3:
		info->trc = "bt709";
		info->av_trc = AVCOL_TRC_BT709;
		break;
	case 4:
	case 8:
		info->trc = "iec61966-2-1";
		info->av_trc = AVCOL_TRC_IEC61966_2_1;
		break;
	case 5:
		info->trc = "arib-std-b67";
		info->av_trc = AVCOL_TRC_ARIB_STD_B67;
		break;
	case 6:
	case 7:
		info->trc = "smpte2084";
		info->av_trc = AVCOL_TRC_SMPTE2084;
		break;
	case 9:
		info->trc = "linear";
		info->av_trc = AVCOL_TRC_LINEAR;
		break;
	default:
		return false;
	}

	info->hdr = gamma == 5 || gamma == 6 || gamma == 7;
	info->output_format = info->hdr ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
	return true;
}

static bool frame_color_info(const AVFrame *frame, struct cst_color_info *info)
{
	if (!frame || !info)
		return false;

	*info = (struct cst_color_info){0};

	if (frame->color_primaries == AVCOL_PRI_BT2020 ||
	    frame->colorspace == AVCOL_SPC_BT2020_NCL ||
	    frame->colorspace == AVCOL_SPC_BT2020_CL) {
		info->primaries = "bt2020";
		info->av_primaries = AVCOL_PRI_BT2020;
	} else {
		info->primaries = "bt709";
		info->av_primaries = AVCOL_PRI_BT709;
	}

	switch (frame->colorspace) {
	case AVCOL_SPC_BT2020_CL:
		info->matrix = "2020_cl";
		info->av_space = AVCOL_SPC_BT2020_CL;
		break;
	case AVCOL_SPC_BT2020_NCL:
		info->matrix = "2020_ncl";
		info->av_space = AVCOL_SPC_BT2020_NCL;
		break;
	case AVCOL_SPC_BT709:
		info->matrix = "709";
		info->av_space = AVCOL_SPC_BT709;
		break;
	default:
		info->matrix = info->av_primaries == AVCOL_PRI_BT2020 ? "2020_ncl" : "709";
		info->av_space = info->av_primaries == AVCOL_PRI_BT2020 ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
		break;
	}

	switch (frame->color_trc) {
	case AVCOL_TRC_GAMMA22:
		info->trc = "bt470m";
		info->av_trc = AVCOL_TRC_GAMMA22;
		break;
	case AVCOL_TRC_SMPTE170M:
		info->trc = "smpte170m";
		info->av_trc = AVCOL_TRC_SMPTE170M;
		break;
	case AVCOL_TRC_BT709:
	case AVCOL_TRC_UNSPECIFIED:
		info->trc = "bt709";
		info->av_trc = AVCOL_TRC_BT709;
		break;
	case AVCOL_TRC_IEC61966_2_1:
		info->trc = "iec61966-2-1";
		info->av_trc = AVCOL_TRC_IEC61966_2_1;
		break;
	case AVCOL_TRC_LINEAR:
		info->trc = "linear";
		info->av_trc = AVCOL_TRC_LINEAR;
		break;
	case AVCOL_TRC_ARIB_STD_B67:
		info->trc = "arib-std-b67";
		info->av_trc = AVCOL_TRC_ARIB_STD_B67;
		break;
	case AVCOL_TRC_SMPTE2084:
		info->trc = "smpte2084";
		info->av_trc = AVCOL_TRC_SMPTE2084;
		break;
	case AVCOL_TRC_BT2020_10:
		info->trc = "bt2020-10";
		info->av_trc = AVCOL_TRC_BT2020_10;
		break;
	case AVCOL_TRC_BT2020_12:
		info->trc = "bt2020-12";
		info->av_trc = AVCOL_TRC_BT2020_12;
		break;
	default:
		info->trc = "bt709";
		info->av_trc = AVCOL_TRC_BT709;
		break;
	}

	info->hdr = frame->color_trc == AVCOL_TRC_ARIB_STD_B67 || frame->color_trc == AVCOL_TRC_SMPTE2084 ||
		    frame->color_trc == AVCOL_TRC_BT2020_10 || frame->color_trc == AVCOL_TRC_BT2020_12;
	info->output_format = AV_PIX_FMT_YUV420P;
	return true;
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

static bool create_filter(AVFilterGraph *graph, const char *name, const char *instance, const char *args,
				  AVFilterContext **out)
{
	const AVFilter *filter = avfilter_get_by_name(name);
	int ret;

	if (!filter) {
		obs_log(LOG_ERROR, "scrcpy-color-transform: filter '%s' is unavailable", name);
		return false;
	}

	ret = avfilter_graph_create_filter(out, filter, instance, args, NULL, graph);
	if (ret < 0) {
		char error_string[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, error_string, sizeof(error_string));
		obs_log(LOG_ERROR, "scrcpy-color-transform: creating %s failed: %s (args=%s)", name, error_string,
			args ? args : "");
		return false;
	}

	return true;
}

static bool link_filters(AVFilterContext *src, AVFilterContext *dst, const char *stage)
{
	int ret = avfilter_link(src, 0, dst, 0);
	if (ret >= 0)
		return true;

	char error_string[AV_ERROR_MAX_STRING_SIZE];
	av_strerror(ret, error_string, sizeof(error_string));
	obs_log(LOG_ERROR, "scrcpy-color-transform: linking %s failed: %s", stage, error_string);
	return false;
}

static bool get_source_color_info(const scrcpy_color_transform_t *transform, const AVFrame *input,
				       struct cst_color_info *info)
{
	if (transform->source_profile > 0)
		return profile_to_color_info(transform->source_profile, info);

	return frame_color_info(input, info);
}

static bool build_graph(scrcpy_color_transform_t *transform, const AVFrame *input, enum AVColorRange input_range)
{
	struct cst_color_info source;
	struct cst_color_info target;
	AVFilterContext *current = NULL;
	AVFilterContext *format_filter = NULL;
	char buffer_args[256];
	char zscale_args[1024];
	int ret;

	if (!get_source_color_info(transform, input, &source))
		return false;

	if (!profile_to_color_info(transform->target_profile, &target))
		return false;

	avfilter_graph_free(&transform->graph);
	transform->buffer_src = NULL;
	transform->buffer_sink = NULL;

	transform->graph = avfilter_graph_alloc();
	if (!transform->graph)
		return false;

	const enum AVPixelFormat source_format = (enum AVPixelFormat)input->format;
	const enum AVPixelFormat input_format = normalize_input_format(source_format);
	const char *source_pix_fmt_name = av_get_pix_fmt_name(source_format);
	const char *input_pix_fmt_name = av_get_pix_fmt_name(input_format);
	const char *range_name = input_range == AVCOL_RANGE_JPEG ? "full" : "limited";
	if (!source_pix_fmt_name || !input_pix_fmt_name)
		goto fail;

	snprintf(buffer_args, sizeof(buffer_args), "video_size=%dx%d:pix_fmt=%s:time_base=1/1000000:pixel_aspect=1/1",
		 input->width, input->height, source_pix_fmt_name);

	if (!create_filter(transform->graph, "buffer", "in", buffer_args, &transform->buffer_src))
		goto fail;
	current = transform->buffer_src;

	if (input_format != source_format) {
		char args[128];
		snprintf(args, sizeof(args), "pix_fmts=%s", input_pix_fmt_name);
		if (!create_filter(transform->graph, "format", "normalize", args, &format_filter))
			goto fail;
		if (!link_filters(current, format_filter, "normalize"))
			goto fail;
		current = format_filter;
	}

	const bool hdr_to_sdr = source.hdr && !target.hdr;
	if (hdr_to_sdr) {
		AVFilterContext *linear = NULL;
		AVFilterContext *float_format = NULL;
		AVFilterContext *tone_map = NULL;
		AVFilterContext *target_filter = NULL;

		snprintf(zscale_args, sizeof(zscale_args),
			 "primariesin=%s:matrixin=%s:transferin=%s:rangein=%s:primaries=%s:matrix=gbr:transfer=linear:range=full",
			 source.primaries, source.matrix, source.trc, range_name, source.primaries);

		if (!create_filter(transform->graph, "zscale", "to-linear-rgb", zscale_args, &linear))
			goto fail;
		if (!link_filters(current, linear, "HDR to linear RGB"))
			goto fail;
		current = linear;

		if (!create_filter(transform->graph, "format", "float-rgb", "pix_fmts=gbrpf32le", &float_format))
			goto fail;
		if (!link_filters(current, float_format, "linear RGB format"))
			goto fail;
		current = float_format;

		if (!create_filter(transform->graph, "tonemap", "tone-map", "tonemap=mobius:param=0.3:desat=2", &tone_map))
			goto fail;
		if (!link_filters(current, tone_map, "tone map"))
			goto fail;
		current = tone_map;

		snprintf(zscale_args, sizeof(zscale_args),
			 "primariesin=%s:matrixin=gbr:transferin=linear:rangein=full:primaries=%s:matrix=%s:transfer=%s:range=limited",
			 source.primaries, target.primaries, target.matrix, target.trc);

		if (!create_filter(transform->graph, "zscale", "to-target-sdr", zscale_args, &target_filter))
			goto fail;
		if (!link_filters(current, target_filter, "linear RGB to target SDR"))
			goto fail;
		current = target_filter;
	} else {
		AVFilterContext *zscale = NULL;

		snprintf(zscale_args, sizeof(zscale_args),
			 "primariesin=%s:matrixin=%s:transferin=%s:rangein=%s:primaries=%s:matrix=%s:transfer=%s:range=limited",
			 source.primaries, source.matrix, source.trc, range_name, target.primaries, target.matrix, target.trc);

		if (!create_filter(transform->graph, "zscale", "color-transform", zscale_args, &zscale))
			goto fail;
		if (!link_filters(current, zscale, "color transform"))
			goto fail;
		current = zscale;
	}

	{
		AVFilterContext *output_format = NULL;
		const char *target_format_name = av_get_pix_fmt_name(target.output_format);
		if (!target_format_name)
			goto fail;

		char args[128];
		snprintf(args, sizeof(args), "pix_fmts=%s", target_format_name);
		if (!create_filter(transform->graph, "format", "output-format", args, &output_format))
			goto fail;
		if (!link_filters(current, output_format, "output format"))
			goto fail;
		current = output_format;
	}

	{
		AVFilterContext *sink = NULL;
		if (!create_filter(transform->graph, "buffersink", "out", NULL, &sink))
			goto fail;
		if (!link_filters(current, sink, "sink"))
			goto fail;

		ret = avfilter_graph_config(transform->graph, NULL);
		if (ret < 0) {
			char error_string[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, error_string, sizeof(error_string));
			obs_log(LOG_ERROR, "scrcpy-color-transform: configuring graph failed: %s", error_string);
			goto fail;
		}

		transform->buffer_sink = sink;
	}

	transform->input_format = source_format;
	transform->input_range = input_range;
	transform->input_width = input->width;
	transform->input_height = input->height;

	obs_log(LOG_INFO, "scrcpy-color-transform: source=%s/%s/%s %s -> target=%s/%s/%s %s, input=%s, output=%s",
		source.primaries, source.matrix, source.trc, range_name, target.primaries, target.matrix, target.trc,
		target.hdr ? "10-bit" : "8-bit", source_pix_fmt_name, av_get_pix_fmt_name(target.output_format));

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
	transform->input_range = AVCOL_RANGE_UNSPECIFIED;
	return transform;
}

bool scrcpy_color_transform_apply(scrcpy_color_transform_t *transform, AVFrame *input, AVFrame *output,
				   enum AVColorRange input_range)
{
	struct cst_color_info target;
	int ret;

	if (!transform || !input || !output || !profile_to_color_info(transform->target_profile, &target))
		return false;

	if (input_range != AVCOL_RANGE_JPEG && input_range != AVCOL_RANGE_MPEG)
		input_range = input->color_range == AVCOL_RANGE_JPEG ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;

	if (!transform->graph || transform->input_format != (enum AVPixelFormat)input->format ||
	    transform->input_range != input_range || transform->input_width != input->width ||
	    transform->input_height != input->height) {
		if (!build_graph(transform, input, input_range))
			return false;
	}

	ret = av_buffersrc_add_frame_flags(transform->buffer_src, input, AV_BUFFERSRC_FLAG_KEEP_REF);
	if (ret < 0) {
		char error_string[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, error_string, sizeof(error_string));
		obs_log(LOG_ERROR, "scrcpy-color-transform: push frame failed: %s", error_string);
		return false;
	}

	av_frame_unref(output);
	ret = av_buffersink_get_frame(transform->buffer_sink, output);
	if (ret < 0) {
		char error_string[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, error_string, sizeof(error_string));
		obs_log(LOG_ERROR, "scrcpy-color-transform: pull frame failed: %s", error_string);
		return false;
	}

	output->color_primaries = target.av_primaries;
	output->colorspace = target.av_space;
	output->color_trc = target.av_trc;
	output->color_range = AVCOL_RANGE_MPEG;

	if ((enum AVPixelFormat)output->format != target.output_format) {
		obs_log(LOG_ERROR, "scrcpy-color-transform: output format mismatch: got %s, expected %s",
			av_get_pix_fmt_name((enum AVPixelFormat)output->format), av_get_pix_fmt_name(target.output_format));
		av_frame_unref(output);
		return false;
	}

	return true;
}

bool scrcpy_color_transform_target_is_hdr(int target_profile)
{
	struct cst_color_info target;
	return profile_to_color_info(target_profile, &target) && target.hdr;
}

void scrcpy_color_transform_destroy(scrcpy_color_transform_t *transform)
{
	if (!transform)
		return;

	avfilter_graph_free(&transform->graph);
	av_free(transform);
}
