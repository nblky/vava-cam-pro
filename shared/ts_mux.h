#ifndef VAVA_TS_MUX_H
#define VAVA_TS_MUX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* H.264 I/P access units and AAC-LC ADTS only. No decoding or invented FPS.
 * All input timestamps use the camera's common millisecond clock. */
struct ts_mux {
    uint64_t first_ms, last_ms, video_pts, audio_pts, audio_frames;
    unsigned sample_rate, channels;
    uint8_t cc[4];
    /* Event clips remain bounded; live streams opt in to a continuous clock. */
    bool started, audio, continuous;
    const char *error;
};
typedef bool (*ts_write_fn)(void *, const uint8_t *, size_t);
bool ts_mux_frame(struct ts_mux *, bool audio, bool keyframe, uint64_t timestamp_ms,
                  const uint8_t *, size_t, ts_write_fn, void *);
#endif
