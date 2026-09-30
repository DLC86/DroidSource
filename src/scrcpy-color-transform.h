#pragma once

#include <libavutil/frame.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A CST target uses the same packed color-profile IDs as the camera
 * color-space/gamma selector:
 *
 *     color space * 10 + transfer/gamma
 *
 * SCRCPY_CST_OFF (0) disables the transform entirely.
 */
#define SCRCPY_CST_OFF 0

typedef struct scrcpy_color_transform scrcpy_color_transform_t;

scrcpy_color_transform_t *scrcpy_color_transform_create(int source_profile, int target_profile);

bool scrcpy_color_transform_apply(scrcpy_color_transform_t *transform, AVFrame *input, AVFrame *output);

void scrcpy_color_transform_destroy(scrcpy_color_transform_t *transform);

bool scrcpy_color_transform_target_is_hdr(int target_profile);

#ifdef __cplusplus
}
#endif
