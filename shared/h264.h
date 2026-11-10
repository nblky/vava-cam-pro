#ifndef VAVA_H264_H
#define VAVA_H264_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Validate Annex B framing, supported NAL types and byte escaping. This is a
 * bounded syntax check, not a replacement for an H.264 entropy decoder. */
bool h264_valid(const uint8_t *,size_t);
bool h264_dimensions(const uint8_t *,size_t,unsigned *width,unsigned *height);
/* For an already validated progressive I/P-only stream: explicitly declare
 * zero frame reordering when the SPS omits that VUI restriction. Preserve
 * reference buffering and all image/timing/colour fields. Zero on unsupported
 * syntax, an explicit nonzero reorder requirement, or insufficient capacity. */
size_t h264_sps_low_delay(const uint8_t *,size_t,uint8_t *out,size_t capacity);
/* avc1 carries parameter sets in avcC. Remove their in-band copies so they
 * cannot override the normalized initialization; preserve VCL/SEI/AUD bytes. */
size_t h264_without_parameter_sets(const uint8_t *,size_t,uint8_t *out,size_t capacity);
/* Bounded parameter-set cache. A new epoch commits only on an IDR whose PPS
 * and SPS references resolve. Connection identity is deliberately separate. */
struct h264_set {uint32_t id,parent;size_t size;uint8_t data[1024];bool valid;};
struct h264_config {
    struct h264_set sps[4],pps[4];
    uint8_t active_sps[1024],active_pps[1024];
    size_t sps_size,pps_size;
    uint32_t epoch,pps_id;unsigned width,height;
};
/* -1 malformed/unsupported, 0 wait for matching IDR, 1 accepted VCL AU.
 * Repeated headers/IDRs do not advance epoch. I/P only; no invented DTS. */
int h264_config_frame(struct h264_config *,const uint8_t *,size_t,bool *key,bool *changed);
#endif
