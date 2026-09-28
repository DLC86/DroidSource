#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct scrcpy_camera_control scrcpy_camera_control_t;

scrcpy_camera_control_t *scrcpy_camera_control_create(const char *serial, uint16_t port);
void scrcpy_camera_control_destroy(scrcpy_camera_control_t *control);

bool scrcpy_camera_control_apply(scrcpy_camera_control_t *control,
                                 float zoom, bool torch, int iso, int shutter_us,
                                 float focus_distance, int wb_kelvin);
