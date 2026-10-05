#pragma once

#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
typedef struct scrcpy_d3d11_cst scrcpy_d3d11_cst_t;

scrcpy_d3d11_cst_t *scrcpy_d3d11_cst_create(AVBufferRef *hw_device_ctx, int source_profile, int target_profile);
bool scrcpy_d3d11_cst_apply(scrcpy_d3d11_cst_t *cst, const AVFrame *input, AVFrame *output,
			     enum AVColorRange input_range);
void scrcpy_d3d11_cst_destroy(scrcpy_d3d11_cst_t *cst);
#else
typedef void scrcpy_d3d11_cst_t;

static inline scrcpy_d3d11_cst_t *scrcpy_d3d11_cst_create(AVBufferRef *hw_device_ctx, int source_profile,
							    int target_profile)
{
	(void)hw_device_ctx;
	(void)source_profile;
	(void)target_profile;
	return NULL;
}

static inline bool scrcpy_d3d11_cst_apply(scrcpy_d3d11_cst_t *cst, const AVFrame *input, AVFrame *output,
					  enum AVColorRange input_range)
{
	(void)cst;
	(void)input;
	(void)output;
	(void)input_range;
	return false;
}

static inline void scrcpy_d3d11_cst_destroy(scrcpy_d3d11_cst_t *cst)
{
	(void)cst;
}
#endif

#ifdef __cplusplus
}
#endif
