#include "scrcpy-color-transform.h"

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avstring.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>

#include <stdio.h>
#include <string.h>

struct scrcpy_color_transform {
	int source_profile;
	int target_profile;
	AVFilterGraph *graph;
	AVFilterContext *buffer_src;
	AVFilterContext *buffer_sink;
	AVFrame *frame;
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
	int color_space;
	int gamma;

	if (profile <= 0)
		return false;

	color_space = profile / 10;
	gamma = profile % 10;
	if (color_space < 1 || color_space > 3 || gamma < 1 || gamma > 9)
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
		/* The camera's Gamma 2.4 mode maps to the standard SMPTE 170M
		 * transfer available in FFmpeg's color conversion filters. */
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

	if (!info || profile <= 0 ||
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

static bool source_is_hdr(const struct scrcpy_color_transform *transform, const AVFrame *input)
{
	if (transform->source_profile > 0) {
		int gamma = transform->source_profile % 10;
		return gamma == 5 || gamma == 6 || gamma == 7;
	}

	return input && (input->color_trc == AVCOL_TRC_ARIB_STD_B67 || input->color_trc == AVCOL_TRC_SMPTE2084 ||
			 input->color_trc == AVCOL_TRC_BT2020_10 || input->color_trc == AVCOL_TRC_BT2020_12);
}

static const char *source_override_options(const scrcpy_color_transform_t *transform, char *buf, size_t size)
{
	const char *primaries = NULL;
	const char *matrix = NULL;
	const char *trc = NULL;
	enum AVColorPrimaries av_primaries;
	enum AVColorSpace av_space;
	enum AVColorTransferCharacteristic av_trc;

	if (!transform || transform->source_profile <= 0 ||
	    !profile_to_colorimetry(transform->source_profile, &primaries, &matrix, &trc, &av_primaries, &av_space,
				    &av_trc))
		return "";

	snprintf(buf, size, ":primariesin=%s:matrixin=%s:transferin=%s", primaries, matrix, trc);
	return buf;
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
	AVFilterContext *normalizer = NULL;
	AVFilterContext *first = NULL;
	AVFilterContext *tone_map = NULL;
	AVFilterContext *second = NULL;
	AVFilterContext *output_format = NULL;
	AVFilterContext *sink = NULL;
	AVFilterContext *current = NULL;
	char buffer_args[256];
	char args[1024];
	char source_override[512];
	const enum AVPixelFormat normalized_format = normalize_input_format((enum AVPixelFormat)input->format);
	const bool input_format_needs_normalization = normalized_format != (enum AVPixelFormat)input->format;
	int ret;

	if (!target_info(transform->target_profile, &target))
		return false;

	if (transform->graph)
		avfilter_graph_free(&transform->graph);

	transform->graph = avfilter_graph_alloc();
	transform->buffer_src = NULL;
	transform->buffer_sink = NULL;
	if (!transform->graph)
		return false;

	const char *pix_fmt_name = av_get_pix_fmt_name(normalized_format);
	if (!pix_fmt_name)
		goto fail;

	snprintf(buffer_args, sizeof(buffer_args), "video_size=%dx%d:pix_fmt=%s:time_base=1/1000000:pixel_aspect=1/1",
		 input->width, input->height, pix_fmt_name);
	if (!create_filter(transform->graph, "buffer", "in", buffer_args, &transform->buffer_src))
		goto fail;

	current = transform->buffer_src;

	if (input_format_needs_normalization) {
		snprintf(args, sizeof(args), "pix_fmts=%s", pix_fmt_name);
		if (!create_filter(transform->graph, "format", "normalize", args, &normalizer))
			goto fail;
		if (avfilter_link(current, 0, normalizer, 0) < 0)
			goto fail;
		current = normalizer;
	}

	source_override_options(transform, source_override, sizeof(source_override));

	if (source_is_hdr(transform, input) && !target.hdr) {
		snprintf(args, sizeof(args),
			 "primariesin=%s:matrixin=%s:transferin=%s:primaries=%s:matrix=gbr:transfer=linear:range=pc%s",
			 transform->source_profile > 0 ? strtok(source_override + 1, ":") : "bt2020",
			 transform->source_profile > 0 ? strtok(NULL, ":") : "2020_ncl",
			 transform->source_profile > 0 ? strtok(NULL, ":") : (input->color_trc == AVCOL_TRC_SMPTE2084 ? "smpte2084" : "arib-std-b67"),
			 source_is_hdr(transform, input) ? "bt2020" : target.primaries, source_override);
		/* The parser above is intentionally not used for explicit profiles below;
		 * rebuild with a direct option string to avoid mutating source_override. */
		{
			const char *in_primaries = "bt2020";
			const char *in_matrix = "2020_ncl";
			const char *in_trc = input->color_trc == AVCOL_TRC_SMPTE2084 ? "smpte2084" : "arib-std-b67";
			if (transform->source_profile > 0) {
				static char p[32];
				static char m[32];
				static char t[32];
				enum AVColorPrimaries ap;
				enum AVColorSpace as;
				enum AVColorTransferCharacteristic at;
				profile_to_colorimetry(transform->source_profile, &in_primaries, &in_matrix, &in_trc, &ap, &as, &at);
				(void)p;
				(void)m;
				(void)t;
			}
			snprintf(args, sizeof(args),
				 "primariesin=%s:matrixin=%s:transferin=%s:primaries=%s:matrix=gbr:transfer=linear:range=pc",
				 in_primaries, in_matrix, in_trc, in_primaries);
		}
		if (!create_filter(transform->graph, "zscale", "hdr-linearize", args, &first))
			goto fail;
		if (avfilter_link(current, 0, first, 0) < 0)
			goto fail;
		current = first;

		if (!create_filter(transform->graph, "format", "float-rgb", "pix_fmts=gbrpf32le", &normalizer))
			goto fail;
		if (avfilter_link(current, 0, normalizer, 0) < 0)
			goto fail;
		current = normalizer;

		if (!create_filter(transform->graph, "tonemap", "tone-map", "tonemap=mobius:param=0.3:desat=2", &tone_map))
			goto fail;
		if (avfilter_link(current, 0, tone_map, 0) < 0)
			goto fail;
		current = tone_map;

		snprintf(args, sizeof(args),
			 "primariesin=%s:matrixin=gbr:transferin=linear:primaries=%s:matrix=%s:transfer=linear:rangein=pc:range=tv",
			 target.primaries == NULL ? "bt709" : "bt2020", target.primaries, target.matrix);
		if (!create_filter(transform->graph, "zscale", "linear-target", args, &second))
			goto fail;
		if (avfilter_link(current, 0, second, 0) < 0)
			goto fail;
		current = second;

		if (!create_filter(transform->graph, "format", "linear-yuv10", "pix_fmts=yuv420p10le", &output_format))
			goto fail;
		if (avfilter_link(current, 0, output_format, 0) < 0)
			goto fail;
		current = output_format;

		snprintf(args, sizeof(args),
			 "iprimaries=%s:ispace=%s:itrc=linear:irange=tv:primaries=%s:space=%s:trc=%s:range=tv:format=yuv420p:dither=fsb",
			 target.primaries, target.matrix, target.primaries, target.matrix, target.trc);
		if (!create_filter(transform->graph, "colorspace", "target-trc", args, &first))
			goto fail;
		if (avfilter_link(current, 0, first, 0) < 0)
			goto fail;
		current = first;
	} else if (target.hdr) {
		char in_primaries[32];
		char in_matrix[32];
		char in_trc[32];
		const char *src_primaries;
		const char *src_matrix;
		const char *src_trc;
		enum AVColorPrimaries ap;
		enum AVColorSpace as;
		enum AVColorTransferCharacteristic at;

		src_primaries = "bt709";
		src_matrix = "709";
		src_trc = "srgb";
		if (transform->source_profile > 0) {
			profile_to_colorimetry(transform->source_profile, &src_primaries, &src_matrix, &src_trc, &ap, &as, &at);
		} else {
			if (input->color_primaries == AVCOL_PRI_BT2020)
				src_primaries = "bt2020";
			if (input->colorspace == AVCOL_SPC_BT2020_NCL || input->colorspace == AVCOL_SPC_BT2020_CL)
				src_matrix = "2020_ncl";
			else if (input->colorspace == AVCOL_SPC_BT709)
				src_matrix = "709";
			switch (input->color_trc) {
			case AVCOL_TRC_BT709:
				src_trc = "bt709";
				break;
			case AVCOL_TRC_GAMMA22:
				src_trc = "gamma22";
				break;
			case AVCOL_TRC_SMPTE170M:
				src_trc = "smpte170m";
				break;
			case AVCOL_TRC_IEC61966_2_1:
				src_trc = "srgb";
				break;
			case AVCOL_TRC_LINEAR:
				src_trc = "linear";
				break;
			case AVCOL_TRC_ARIB_STD_B67:
				src_trc = "arib-std-b67";
				break;
			case AVCOL_TRC_SMPTE2084:
				src_trc = "smpte2084";
				break;
			default:
				break;
			}
		}
		snprintf(in_primaries, sizeof(in_primaries), "%s", src_primaries);
		snprintf(in_matrix, sizeof(in_matrix), "%s", src_matrix);
		snprintf(in_trc, sizeof(in_trc), "%s", src_trc);
		snprintf(args, sizeof(args),
			 "primariesin=%s:matrixin=%s:transferin=%s:primaries=%s:matrix=%s:transfer=%s:range=tv",
			 in_primaries, in_matrix, in_trc, target.primaries, target.matrix, target.trc);
		if (!create_filter(transform->graph, "zscale", "hdr-target", args, &first))
			goto fail;
		if (avfilter_link(current, 0, first, 0) < 0)
			goto fail;
		current = first;
		if (!create_filter(transform->graph, "format", "target-format", "pix_fmts=yuv420p10le", &output_format))
			goto fail;
		if (avfilter_link(current, 0, output_format, 0) < 0)
			goto fail;
		current = output_format;
	} else {
		char src_primaries[32];
		char src_matrix[32];
		char src_trc[32];
		const char *src_primaries_ptr = "bt709";
		const char *src_matrix_ptr = "709";
		const char *src_trc_ptr = "srgb";
		enum AVColorPrimaries ap;
		enum AVColorSpace as;
		enum AVColorTransferCharacteristic at;

		if (transform->source_profile > 0) {
			profile_to_colorimetry(transform->source_profile, &src_primaries_ptr, &src_matrix_ptr, &src_trc_ptr, &ap,
					       &as, &at);
		} else {
			if (input->color_primaries == AVCOL_PRI_BT2020)
				src_primaries_ptr = "bt2020";
			if (input->colorspace == AVCOL_SPC_BT2020_NCL || input->colorspace == AVCOL_SPC_BT2020_CL)
				src_matrix_ptr = "2020_ncl";
			else if (input->colorspace == AVCOL_SPC_BT709)
				src_matrix_ptr = "709";
			switch (input->color_trc) {
			case AVCOL_TRC_BT709:
				src_trc_ptr = "bt709";
				break;
			case AVCOL_TRC_GAMMA22:
				src_trc_ptr = "gamma22";
				break;
			case AVCOL_TRC_SMPTE170M:
				src_trc_ptr = "smpte170m";
				break;
			case AVCOL_TRC_IEC61966_2_1:
				src_trc_ptr = "srgb";
				break;
			case AVCOL_TRC_LINEAR:
				src_trc_ptr = "linear";
				break;
			default:
				break;
			}
		}
		snprintf(src_primaries, sizeof(src_primaries), "%s", src_primaries_ptr);
		snprintf(src_matrix, sizeof(src_matrix), "%s", src_matrix_ptr);
		snprintf(src_trc, sizeof(src_trc), "%s", src_trc_ptr);
		snprintf(args, sizeof(args),
			 "iprimaries=%s:ispace=%s:itrc=%s:irange=input:primaries=%s:space=%s:trc=%s:range=tv:format=%s:dither=fsb",
			 src_primaries, src_matrix, src_trc, target.primaries, target.matrix, target.trc,
			 target.output_format == AV_PIX_FMT_YUV420P10LE ? "yuv420p10le" : "yuv420p");
		if (!create_filter(transform->graph, "colorspace", "sdr-target", args, &first))
			goto fail;
		if (avfilter_link(current, 0, first, 0) < 0)
			goto fail;
		current = first;
	}

	if (!create_filter(transform->graph, "buffersink", "out", NULL, &sink))
		goto fail;
	if (avfilter_link(current, 0, sink, 0) < 0)
		goto fail;

	ret = avfilter_graph_config(transform->graph, NULL);
	if (ret < 0)
		goto fail;

	transform->buffer_sink = sink;
	transform->input_format = normalized_format;
	transform->input_width = input->width;
	transform->input_height = input->height;
	return true;

fail:
	avfilter_graph_free(&transform->graph);
	transform->graph = NULL;
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
	transform->frame = av_frame_alloc();
	if (!transform->frame) {
		av_free(transform);
		return NULL;
	}
	return transform;
}

bool scrcpy_color_transform_apply(scrcpy_color_transform_t *transform, AVFrame *input, AVFrame *output)
{
	const int input_format = input ? input->format : -1;
	int ret;

	if (!transform || !input || !output)
		return false;

	if (!transform->graph || transform->input_format != input_format || transform->input_width != input->width ||
	    transform->input_height != input->height) {
		if (!build_graph(transform, input))
			return false;
	}

	av_frame_unref(output);
	ret = av_buffersrc_add_frame_flags(transform->buffer_src, input, AV_BUFFERSRC_FLAG_KEEP_REF);
	if (ret < 0)
		return false;

	ret = av_buffersink_get_frame(transform->buffer_sink, output);
	if (ret < 0)
		return false;

	output->color_primaries = target_info(transform->target_profile, &(struct cst_target_info){0})
				      ? ((struct cst_target_info){0}).av_primaries
				      : output->color_primaries;
	{
		struct cst_target_info target;
		if (target_info(transform->target_profile, &target)) {
			output->color_primaries = target.av_primaries;
			output->colorspace = target.av_space;
			output->color_trc = target.av_trc;
			output->color_range = AVCOL_RANGE_MPEG;
		}
	}
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
	av_frame_free(&transform->frame);
	av_free(transform);
}
