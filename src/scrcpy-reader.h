#pragma once

#include <obs-module.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reads the patched scrcpy child's raw video packet stream from
 * 127.0.0.1:<port>. Wire format (per packet, big-endian):
 *
 *     [pts: u64 | flags: u8 | size: u32 | NAL bytes...]
 *
 * flags bit 0 = config (SPS/PPS extradata), bit 1 = keyframe.
 *
 * Feeds NAL units to libavcodec, pushes decoded frames to the OBS
 * source via obs_source_output_video().
 */
typedef struct scrcpy_reader scrcpy_reader_t;

scrcpy_reader_t *scrcpy_reader_create(obs_source_t *source, uint16_t port);

void scrcpy_reader_destroy(scrcpy_reader_t *r);
bool scrcpy_reader_is_alive(const scrcpy_reader_t *r);
bool scrcpy_reader_is_stale(const scrcpy_reader_t *r, uint32_t max_age_ms);

#ifdef __cplusplus
}
#endif
