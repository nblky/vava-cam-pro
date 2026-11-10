Source: https://github.com/lieff/minimp4
Commit: 2a3014a7753cfcba4442adeb469431967238fe6a
License: CC0-1.0, dedication in minimp4.h.
Local change: enable MP4D_TFDT_SUPPORT.
The VAVA adapter uses explicit per-track timestamps with the fragment writer; it does not use the upstream constant-duration/global-fragment-counter timestamp approximation in MP4E_put_sample. Only validated self-produced H.264 I/P + AAC TS is accepted. No upstream MP4 demuxer or NAL rewriter is used.
