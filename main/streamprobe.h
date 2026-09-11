/*
 * streamprobe.h -- a throwaway measurement, not a feature.
 *
 * v0.4.0 is internet radio, and before any of the stream path is written
 * three things want measuring on this board: what a real station's HTTP
 * looks like (redirects, headers, ICY), what HTTPS costs in time and
 * internal heap, and what the SDIO-attached C6 actually delivers in bytes
 * per second. The first stream anyone plans to use is WNZK on Zeno.FM.
 *
 * Once per boot, a few seconds after the station first gets an address,
 * this connects to STREAMPROBE_URL, follows redirects by hand so each hop
 * is logged, requests ICY metadata, and reads the body for
 * STREAMPROBE_SECONDS, logging throughput every five seconds, what the
 * first bytes are, and the first stream titles. Then it closes and ends.
 * It decodes nothing and draws nothing, and playback carries on beside it
 * -- which is part of what is being measured.
 *
 * Empty STREAMPROBE_URL compiles it to nothing. It goes when the stream
 * path exists.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define STREAMPROBE_URL      "https://stream.zeno.fm/erunhwj5lekvv"
#define STREAMPROBE_SECONDS  (60)
/*
 * Seconds after the first address before the probe starts. Long enough
 * to start a track from the card by hand, so the second run measures the
 * download beside playback, which is what the stream path will have to do.
 */
#define STREAMPROBE_DELAY_S  (45)

/* Called when the station has an address. Starts the probe the first
 * time only; cheap and safe from the event task. */
void streamprobe_kick(void);

#ifdef __cplusplus
}
#endif
