#ifndef VAVA_RTSP_H
#define VAVA_RTSP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTSP_INPUT_LIMIT 8192
struct rtsp_request {
    char method[24], uri[512], transport[256], session[64];
    char authorization[256];
    unsigned cseq;
};
struct rtsp_session {
    bool demand, setup, playing, describing, timed;
    unsigned channel, cseq;
    uint64_t deadline, source_ms, local_ms, report_at;
    uint16_t sequence;
    uint32_t ssrc, timestamp, packets, octets;
    char id[17];
};
/* 0 incomplete, 1 complete, -1 malformed/unsupported body. */
int rtsp_parse(const uint8_t *data, size_t n, struct rtsp_request *out, size_t *used);
bool rtsp_path(const char *uri, unsigned *slot, unsigned *stream, bool *track);
bool rtsp_transport(const char *text, unsigned *channel);
int rtsp_sdp(char *out, size_t cap, const uint8_t *sps, size_t ns, const uint8_t *pps, size_t np);
bool rtsp_rtcp_valid(const uint8_t *data, size_t n);
typedef bool (*rtsp_emit)(void *opaque, const void *data, size_t n);
/* Annex B access unit -> RFC 6184 single NAL/FU-A over interleaved TCP. */
bool rtsp_h264(struct rtsp_session *s, const uint8_t *data, size_t n,
               bool marker, rtsp_emit emit, void *opaque);
bool rtsp_report(struct rtsp_session *s, uint64_t unix_ms, rtsp_emit emit, void *opaque);
#endif
