#ifndef MYVAVA_MP4_ARCHIVE_H
#define MYVAVA_MP4_ARCHIVE_H
#include "files.h"
/* Converts only TS produced by our recorder. Caller verifies source digest and
 * publishes destination atomically after success. Uses bounded <4 MiB scratch;
 * no decoder, subprocess, network protocol, or plaintext local temporary file. */
bool mp4_archive(int source,int destination,const struct myvava_file *);
#endif
