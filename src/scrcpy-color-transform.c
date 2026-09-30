#include "scrcpy-color-transform.h"

#include <obs-module.h>
#include <plugin-support.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>

#include <stdint.h>
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
	int transfer_id;
	enum AVPixelFormat output_format;
};

static bool profile_to_color_info(int profile, struct cst_color_info *info)
{
	int encoded_profile = profile;
	bool output_8bit = false;
	int color_space;
	int gamma;

	/*
	 * Camera profiles keep their original compact IDs. CST-only 8-bit HLG
	 * profiles use the 1000 range so the bit depth is encoded explicitly.
	 */
	if (profile >= 1000 && profile < 1100) {
		encoded_profile = profile - 1000;
		output_8bit = true;
	}

	color_space = encoded_profile / 10;
	gamma = encoded_profile % 10;

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
		info->transfer_id = 1;
		break;
	case 2:
		info->trc = "smpte170m";
		info->av_trc = AVCOL_TRC_SMPTE170M;
		info->transfer_id = 2;
		break;
	case 3:
		info->trc = "bt709";
		info->av_trc = AVCOL_TRC_BT709;
		info->transfer_id = 3;
		break;
	case 4:
	case 8:
		info->trc = "iec61966-2-1";
		info->av_trc = AVCOL_TRC_IEC61966_2_1;
		info->transfer_id = 4;
		break;
	case 5:
		info->trc = "arib-std-b67";
		info->av_trc = AVCOL_TRC_ARIB_STD_B67;
		info->transfer_id = 5;
		break;
	case 6:
	case 7:
		info->trc = "smpte2084";
		info->av_trc = AVCOL_TRC_SMPTE2084;
		info->transfer_id = 6;
		break;
	case 9:
		info->trc = "linear";
		info->av_trc = AVCOL_TRC_LINEAR;
		info->transfer_id = 9;
		break;
	default:
		return false;
	}

	info->hdr = gamma == 5 || gamma == 6 || gamma == 7;
	/* This field is used for the CST destination. Source precision is taken
	 * from the actual AVFrame and must never be inferred from the profile. */
	info->output_format = output_8bit ? AV_PIX_FMT_YUV420P
			      : info->hdr ? AV_PIX_FMT_YUV420P10LE
					  : AV_PIX_FMT_YUV420P;
	return true;
}

static bool frame_color_info(const AVFrame *frame, struct cst_color_info *info)
{
	if (!frame || !info)
		return false;

	*info = (struct cst_color_info){0};

	if (frame->color_primaries == AVCOL_PRI_BT2020 || frame->colorspace == AVCOL_SPC_BT2020_NCL ||
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
		info->transfer_id = 1;
		break;
	case AVCOL_TRC_SMPTE170M:
		info->trc = "smpte170m";
		info->av_trc = AVCOL_TRC_SMPTE170M;
		info->transfer_id = 2;
		break;
	case AVCOL_TRC_BT709:
	case AVCOL_TRC_UNSPECIFIED:
		info->trc = "bt709";
		info->av_trc = AVCOL_TRC_BT709;
		info->transfer_id = 3;
		break;
	case AVCOL_TRC_IEC61966_2_1:
		info->trc = "iec61966-2-1";
		info->av_trc = AVCOL_TRC_IEC61966_2_1;
		info->transfer_id = 4;
		break;
	case AVCOL_TRC_LINEAR:
		info->trc = "linear";
		info->av_trc = AVCOL_TRC_LINEAR;
		info->transfer_id = 9;
		break;
	case AVCOL_TRC_ARIB_STD_B67:
		info->trc = "arib-std-b67";
		info->av_trc = AVCOL_TRC_ARIB_STD_B67;
		info->transfer_id = 5;
		break;
	case AVCOL_TRC_SMPTE2084:
		info->trc = "smpte2084";
		info->av_trc = AVCOL_TRC_SMPTE2084;
		info->transfer_id = 6;
		break;
	case AVCOL_TRC_BT2020_10:
		/* BT.2020-10 is an SDR transfer characteristic, not PQ. */
		info->trc = "bt2020-10";
		info->av_trc = AVCOL_TRC_BT2020_10;
		info->transfer_id = 3;
		break;
	case AVCOL_TRC_BT2020_12:
		/* BT.2020-12 is an SDR transfer characteristic, not PQ. */
		info->trc = "bt2020-12";
		info->av_trc = AVCOL_TRC_BT2020_12;
		info->transfer_id = 3;
		break;
	default:
		info->trc = "bt709";
		info->av_trc = AVCOL_TRC_BT709;
		info->transfer_id = 3;
		break;
	}

	/* BT.2020-10/12 are SDR transfer characteristics. Only HLG and PQ
	 * enter the HDR/tone-mapping paths. */
	info->hdr = frame->color_trc == AVCOL_TRC_ARIB_STD_B67 || frame->color_trc == AVCOL_TRC_SMPTE2084;
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

static bool add_lutrgb_filter(AVFilterGraph *graph, AVFilterContext *current, const char *instance,
			      const char *expression, AVFilterContext **next)
{
	char args[1024];
	snprintf(args, sizeof(args), "r='%s':g='%s':b='%s'", expression, expression, expression);
	if (!create_filter(graph, "lutrgb", instance, args, next))
		return false;
	return link_filters(current, *next, instance);
}

static bool get_transfer_expression(const struct cst_color_info *info, bool encode, char *expression,
				    size_t expression_size)
{
	if (!info || !expression || expression_size == 0)
		return false;

	switch (info->transfer_id) {
	case 1:
		snprintf(expression, expression_size, encode ? "pow(val/maxval,1/2.2)*maxval"
						    : "pow(val/maxval,2.2)*maxval");
		return true;

	case 2:
		snprintf(expression, expression_size, encode ? "pow(val/maxval,1/2.4)*maxval"
						    : "pow(val/maxval,2.4)*maxval");
		return true;

	case 3:
		if (encode) {
			snprintf(expression, expression_size,
				 "if(lte(val/maxval,0.018)\\,4.5*(val/maxval)*maxval\\,(1.099*pow(val/maxval,0.45)-0.099)*maxval)");
		} else {
			snprintf(expression, expression_size,
				 "if(lte(val/maxval,0.081)\\,(val/maxval)/4.5*maxval\\,pow(((val/maxval)+0.099)/1.099,1/0.45)*maxval)");
		}
		return true;

	case 4:
		if (encode) {
			snprintf(expression, expression_size,
				 "if(lte(val/maxval,0.0031308)\\,12.92*(val/maxval)*maxval\\,(1.055*pow(val/maxval,1/2.4)-0.055)*maxval)");
		} else {
			snprintf(expression, expression_size,
				 "if(lte(val/maxval,0.04045)\\,(val/maxval)/12.92*maxval\\,pow(((val/maxval)+0.055)/1.055,2.4)*maxval)");
		}
		return true;

	case 5:
		if (encode) {
			snprintf(expression, expression_size,
				 "if(lte(val/maxval,1/12)\\,sqrt(3*(val/maxval))*maxval\\,(0.17883277*log(12*(val/maxval)-0.28466892)+0.55991073)*maxval)");
		} else {
			snprintf(expression, expression_size,
				 "if(lte(val/maxval,0.5)\\,(val/maxval)*(val/maxval)/3*maxval\\,(exp(((val/maxval)-0.55991073)/0.17883277)+0.28466892)/12*maxval)");
		}
		return true;

	case 6:
		if (encode) {
			snprintf(expression, expression_size,
				 "pow((0.8359375+18.8515625*pow(val/maxval,0.1593017578))/(1+18.6875*pow(val/maxval,0.1593017578)),78.84375)*maxval");
		} else {
			snprintf(expression, expression_size,
				 "pow(max(pow(val/maxval,1/78.84375)-0.8359375\\,0)/(18.8515625-18.6875*pow(val/maxval,1/78.84375))\\,1/0.1593017578)*maxval");
		}
		return true;

	default:
		return false;
	}
}

static bool add_primary_matrix(AVFilterGraph *graph, AVFilterContext *current, const struct cst_color_info *source,
			       const struct cst_color_info *target, AVFilterContext **next)
{
	if (!source || !target || source->av_primaries == target->av_primaries) {
		*next = current;
		return true;
	}

	const char *args;
	if (source->av_primaries == AVCOL_PRI_BT709 && target->av_primaries == AVCOL_PRI_BT2020)
		args = "rr=0.6274039:rg=0.3292830:rb=0.0433131:gr=0.0690973:gg=0.9195404:gb=0.0113623:br=0.0163914:bg=0.0880133:bb=0.8955953";
	else if (source->av_primaries == AVCOL_PRI_BT2020 && target->av_primaries == AVCOL_PRI_BT709)
		args = "rr=1.660491:rg=-0.58764114:rb=-0.07284986:gr=-0.12455047:gg=1.1328999:gb=-0.00834942:br=-0.01815076:bg=-0.1005789:bb=1.11872966";
	else {
		obs_log(LOG_WARNING, "scrcpy-color-transform: unsupported primary conversion %d -> %d",
			source->av_primaries, target->av_primaries);
		*next = current;
		return true;
	}

	if (!create_filter(graph, "colorchannelmixer", "primary-matrix", args, next))
		return false;
	return link_filters(current, *next, "primary matrix");
}

static bool build_graph(scrcpy_color_transform_t *transform, const AVFrame *input, enum AVColorRange input_range)
{
	struct cst_color_info source;
	struct cst_color_info target;
	AVFilterContext *current = NULL;
	AVFilterContext *next = NULL;
	AVFilterContext *sink = NULL;
	char buffer_args[512];
	char filter_args[1024];
	char expression[1024];
	int ret;

	if (!get_source_color_info(transform, input, &source) ||
	    !profile_to_color_info(transform->target_profile, &target))
		return false;

	/* OBS has no independent linear-transfer YUV representation. */
	if (source.transfer_id == 9 || target.transfer_id == 9)
		return false;

	avfilter_graph_free(&transform->graph);
	transform->buffer_src = NULL;
	transform->buffer_sink = NULL;
	transform->graph = avfilter_graph_alloc();
	if (!transform->graph)
		return false;

	const enum AVPixelFormat source_format = (enum AVPixelFormat)input->format;
	const enum AVPixelFormat input_format = normalize_input_format(source_format);
	const AVPixFmtDescriptor *source_desc = av_pix_fmt_desc_get(source_format);
	const bool source_is_10bit =
		source_desc && source_desc->nb_components >= 3 && source_desc->comp[0].depth >= 10;
	const char *rgb_pix_fmt_name = source_is_10bit ? "gbrp16le" : "gbrp";
	const char *target_pix_fmt_name = av_get_pix_fmt_name(target.output_format);
	const char *source_pix_fmt_name = av_get_pix_fmt_name(source_format);
	const char *input_pix_fmt_name = av_get_pix_fmt_name(input_format);
	if (!source_pix_fmt_name || !input_pix_fmt_name || !target_pix_fmt_name)
		goto fail;

	const char *source_space = source.av_primaries == AVCOL_PRI_BT2020 ? "bt2020ncl" : "bt709";
	const char *target_space = target.av_primaries == AVCOL_PRI_BT2020 ? "bt2020ncl" : "bt709";
	const char *range_name = input_range == AVCOL_RANGE_JPEG ? "pc" : "tv";

	snprintf(buffer_args, sizeof(buffer_args),
		 "video_size=%dx%d:pix_fmt=%s:time_base=1/1000000:pixel_aspect=1/1", input->width, input->height,
		 source_pix_fmt_name);
	if (!create_filter(transform->graph, "buffer", "in", buffer_args, &transform->buffer_src))
		goto fail;
	current = transform->buffer_src;

	if (input_format != source_format) {
		snprintf(filter_args, sizeof(filter_args), "pix_fmts=%s", input_pix_fmt_name);
		if (!create_filter(transform->graph, "format", "normalize", filter_args, &next))
			goto fail;
		if (!link_filters(current, next, "normalize"))
			goto fail;
		current = next;
	}

	/*
	 * SDR -> SDR can stay entirely in YUV. colorspace performs matrix,
	 * primaries and transfer conversion and the format option guarantees
	 * the requested 8-bit output.
	 */
	if (!source.hdr && !target.hdr) {
		snprintf(filter_args, sizeof(filter_args),
			 "iprimaries=%s:ispace=%s:itrc=%s:irange=%s:primaries=%s:space=%s:trc=%s:range=tv:"
			 "format=yuv420p:dither=fsb:fast=0:wpadapt=bradford",
			 source.primaries, source_space, source.trc, range_name, target.primaries, target_space,
			 target.trc);
		if (!create_filter(transform->graph, "colorspace", "sdr-cst", filter_args, &next))
			goto fail;
		if (!link_filters(current, next, "SDR CST"))
			goto fail;
		current = next;
	} else if (source.hdr && !target.hdr) {
		/*
		 * HDR -> SDR:
		 * YUV -> RGB (source matrix/range), decode the source transfer to
		 * linear light, tone-map in float, convert primaries in RGB, encode
		 * the destination transfer, then return to explicit YUV420P.
		 */
		snprintf(filter_args, sizeof(filter_args),
			 "in_range=%s:in_color_matrix=%s:out_range=full", range_name,
			 source_space == "bt2020ncl" ? "bt2020" : "bt709");
		if (!create_filter(transform->graph, "scale", "yuv-to-rgb", filter_args, &next))
			goto fail;
		if (!link_filters(current, next, "YUV to RGB"))
			goto fail;
		current = next;

		snprintf(filter_args, sizeof(filter_args), "pix_fmts=%s", rgb_pix_fmt_name);
		if (!create_filter(transform->graph, "format", "rgb", filter_args, &next))
			goto fail;
		if (!link_filters(current, next, source_is_10bit ? "RGB 16-bit" : "RGB 8-bit"))
			goto fail;
		current = next;

		if (!get_transfer_expression(&source, false, expression, sizeof(expression)))
			goto fail;
		if (!add_lutrgb_filter(transform->graph, current, "decode-transfer", expression, &next))
			goto fail;
		current = next;

		if (!create_filter(transform->graph, "format", "rgb-float", "pix_fmts=gbrpf32le", &next))
			goto fail;
		if (!link_filters(current, next, "RGB float"))
			goto fail;
		current = next;

		if (!create_filter(transform->graph, "tonemap", "tone-map",
				   "tonemap=mobius:param=0.3:desat=2", &next))
			goto fail;
		if (!link_filters(current, next, "tone map"))
			goto fail;
		current = next;

		if (!create_filter(transform->graph, "format", "rgb-16bit", "pix_fmts=gbrp16le", &next))
			goto fail;
		if (!link_filters(current, next, "RGB 16-bit after tone map"))
			goto fail;
		current = next;

		if (!add_primary_matrix(transform->graph, current, &source, &target, &next))
			goto fail;
		current = next;

		if (!get_transfer_expression(&target, true, expression, sizeof(expression)))
			goto fail;
		if (!add_lutrgb_filter(transform->graph, current, "encode-transfer", expression, &next))
			goto fail;
		current = next;

		snprintf(filter_args, sizeof(filter_args),
			 "in_range=full:out_range=limited:out_color_matrix=%s",
			 target_space == "bt2020ncl" ? "bt2020" : "bt709");
		if (!create_filter(transform->graph, "scale", "rgb-to-yuv", filter_args, &next))
			goto fail;
		if (!link_filters(current, next, "RGB to YUV"))
			goto fail;
		current = next;
	} else {
		/*
		 * SDR -> HDR and HDR -> HDR:
		 * explicitly enter RGB, decode the source transfer, convert
		 * primaries, encode the requested HDR transfer and enforce the
		 * target's exact 8/10-bit YUV format at the end.
		 */
		snprintf(filter_args, sizeof(filter_args),
			 "in_range=%s:in_color_matrix=%s:out_range=full", range_name,
			 source_space == "bt2020ncl" ? "bt2020" : "bt709");
		if (!create_filter(transform->graph, "scale", "yuv-to-rgb", filter_args, &next))
			goto fail;
		if (!link_filters(current, next, "YUV to RGB"))
			goto fail;
		current = next;

		snprintf(filter_args, sizeof(filter_args), "pix_fmts=%s", rgb_pix_fmt_name);
		if (!create_filter(transform->graph, "format", "rgb", filter_args, &next))
			goto fail;
		if (!link_filters(current, next, source_is_10bit ? "RGB 16-bit" : "RGB 8-bit"))
			goto fail;
		current = next;

		if (source.transfer_id != 9) {
			if (!get_transfer_expression(&source, false, expression, sizeof(expression)))
				goto fail;
			if (!add_lutrgb_filter(transform->graph, current, "decode-transfer", expression, &next))
				goto fail;
			current = next;
		}

		if (!add_primary_matrix(transform->graph, current, &source, &target, &next))
			goto fail;
		current = next;

		if (target.transfer_id != 9) {
			if (!get_transfer_expression(&target, true, expression, sizeof(expression)))
				goto fail;
			if (!add_lutrgb_filter(transform->graph, current, "encode-transfer", expression, &next))
				goto fail;
			current = next;
		}

		/* Make the RGB precision explicit before the 10-bit YUV conversion.
		 * This avoids relying on an automatically inserted scaler during graph
		 * negotiation, which can reject the float/planar RGB -> YUV10 path. */
		if (target.output_format == AV_PIX_FMT_YUV420P10LE) {
			if (!create_filter(transform->graph, "format", "rgb-48", "pix_fmts=rgb48le", &next))
				goto fail;
			if (!link_filters(current, next, "RGB 48-bit"))
				goto fail;
			current = next;
		}

		snprintf(filter_args, sizeof(filter_args),
			 "in_range=full:out_range=limited:out_color_matrix=%s",
			 target_space == "bt2020ncl" ? "bt2020" : "bt709");
		if (!create_filter(transform->graph, "scale", "rgb-to-yuv", filter_args, &next))
			goto fail;
		if (!link_filters(current, next, "RGB to YUV"))
			goto fail;
		current = next;
	}

	snprintf(filter_args, sizeof(filter_args), "pix_fmts=%s", target_pix_fmt_name);
	if (!create_filter(transform->graph, "format", "output-format", filter_args, &next))
		goto fail;
	if (!link_filters(current, next, "output format"))
		goto fail;
	current = next;

	if (!create_filter(transform->graph, "buffersink", "out", NULL, &sink))
		goto fail;
	if (!link_filters(current, sink, "sink"))
		goto fail;

	ret = avfilter_graph_config(transform->graph, NULL);
	if (ret < 0) {
		char error_string[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, error_string, sizeof(error_string));
		obs_log(LOG_ERROR,
			"scrcpy-color-transform: graph config failed: source=%s/%s/%s range=%s target=%s/%s/%s output=%s error=%s",
			source.primaries, source.matrix, source.trc, range_name, target.primaries, target.matrix, target.trc,
			target_pix_fmt_name, error_string);
		goto fail;
	}

	transform->buffer_sink = sink;
	transform->input_format = source_format;
	transform->input_range = input_range;
	transform->input_width = input->width;
	transform->input_height = input->height;

	obs_log(LOG_INFO, "scrcpy-color-transform: source=%s/%s/%s %s -> target=%s/%s/%s %s, input=%s, output=%s",
		source.primaries, source.matrix, source.trc, range_name, target.primaries, target.matrix, target.trc,
		target.hdr ? "HDR" : "SDR", source_pix_fmt_name, target_pix_fmt_name);
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
	if (transform->source_profile > 0 && transform->source_profile == transform->target_profile &&
	    (enum AVPixelFormat)input->format == target.output_format) {
		av_frame_unref(output);
		if (av_frame_ref(output, input) < 0)
			return false;
		output->color_primaries = target.av_primaries;
		output->colorspace = target.av_space;
		output->color_trc = target.av_trc;
		output->color_range = input_range;
		obs_log(LOG_DEBUG, "scrcpy-color-transform: target format=%s (exact-profile bypass)",
			av_get_pix_fmt_name((enum AVPixelFormat)output->format));
		return true;
	}

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
		obs_log(LOG_ERROR, "scrcpy-color-transform: graph produced %s, expected %s",
			av_get_pix_fmt_name((enum AVPixelFormat)output->format),
			av_get_pix_fmt_name(target.output_format));
		av_frame_unref(output);
		return false;
	}

	obs_log(LOG_DEBUG, "scrcpy-color-transform: target format=%s",
		av_get_pix_fmt_name((enum AVPixelFormat)output->format));
	return (enum AVPixelFormat)output->format == target.output_format;
}

bool scrcpy_color_transform_target_is_hdr(int target_profile)
{
	struct cst_color_info target;
	return profile_to_color_info(target_profile, &target) && target.hdr;
}

bool scrcpy_color_transform_target_is_8bit(int target_profile)
{
	struct cst_color_info target;
	return profile_to_color_info(target_profile, &target) && target.output_format == AV_PIX_FMT_YUV420P;
}

void scrcpy_color_transform_destroy(scrcpy_color_transform_t *transform)
{
	if (!transform)
		return;

	avfilter_graph_free(&transform->graph);
	av_free(transform);
}
